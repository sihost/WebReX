/**
 * @file        codegen/phase_pointers.cpp
 * @brief       PointerScan / OrphanBranches phases: entry points reached only through
 *              pointers or tail branches
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Call-graph discovery only finds functions that are reached by `bl` (or listed in .pdata,
// vtables with RTTI, the config). Frameless functions that are only ever reached through a
// pointer (vtable slots without RTTI, `this`-adjusting thunks, callbacks whose address is built
// with lis/addi) or only by a tail branch from another function get no entry, and GapFill
// does not split them off either when the code before them is part of another function (it
// only splits on blr and on `b` to an already known function). A guest indirect call to
// such an address then finds no host function at runtime.
//
// PointerScan (before GapFill) collects code addresses from
//   - every aligned word of the non-executable sections (.rdata/.data/...: vtables, callback
//     tables; relocated words are a subset of these),
//   - lis/addi and lis/ori pairs in code (callbacks passed as arguments or stored),
//   - targets of unresolved non-call branches (tail-call-only targets),
// and registers a target as a function only if it is code that no function covers yet, it
// starts a code region or follows an unconditional terminator (blr, bctr, b) or padding, and
// (for lis-built addresses) the address is not a jump table / used as a load base. Labels
// inside functions (SEH scope entries, switch cases) are covered and so never become entries.
//
// OrphanBranches (after GapFill) registers any remaining tail-branch target that is still in
// no function, so the Validate phase does not fail on it.

#include "codegen_flags.h"
#include "decoded_binary.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBctr = 0x4E800420;

bool isUncondBranch(uint32_t x) {
  return (x >> 26) == 18 && (x & 1) == 0;  // b / ba (no link)
}

bool isUncondTerminator(uint32_t x) {
  return x == kBlr || x == kBctr || isUncondBranch(x);
}

std::optional<uint32_t> readWord(const BinaryView& binary, uint32_t addr) {
  const auto* section = binary.findSection(addr);
  if (!section || !section->data || addr + 4 > section->end())
    return std::nullopt;
  return load_and_swap<uint32_t>(section->data + (addr - section->baseAddress));
}

/// Sorted, merged set of address intervals owned by functions. PDATA/CONFIG/HELPER functions
/// own their declared extent (switch tails and SEH funclets live there), others own their
/// discovered blocks.
class Coverage {
 public:
  explicit Coverage(const FunctionGraph& graph) {
    std::vector<std::pair<uint32_t, uint32_t>> iv;
    iv.reserve(graph.functionCount() * 2);
    for (const auto& [addr, node] : graph.functions()) {
      auto auth = node->authority();
      if (auth == FunctionAuthority::IMPORT)
        continue;
      bool extent = auth == FunctionAuthority::PDATA || auth == FunctionAuthority::CONFIG ||
                    auth == FunctionAuthority::HELPER || node->blocks().empty();
      if (extent) {
        iv.emplace_back(node->base(), node->base() + std::max<uint32_t>(node->size(), 4));
      }
      for (const auto& block : node->blocks()) {
        iv.emplace_back(block.base, block.end());
      }
    }
    std::sort(iv.begin(), iv.end());
    for (const auto& [lo, hi] : iv) {
      if (!merged_.empty() && lo <= merged_.back().second) {
        merged_.back().second = std::max(merged_.back().second, hi);
      } else {
        merged_.emplace_back(lo, hi);
      }
    }
  }

  bool covers(uint32_t addr) const {
    auto it = std::upper_bound(merged_.begin(), merged_.end(), addr,
                               [](uint32_t a, const auto& p) { return a < p.first; });
    if (it == merged_.begin())
      return false;
    --it;
    return addr < it->second;
  }

 private:
  std::vector<std::pair<uint32_t, uint32_t>> merged_;
};

const CodeRegion* findRegion(const std::vector<CodeRegion>& regions, uint32_t addr) {
  auto it = std::upper_bound(regions.begin(), regions.end(), addr,
                             [](uint32_t a, const CodeRegion& r) { return a < r.start; });
  if (it == regions.begin())
    return nullptr;
  --it;
  return it->contains(addr) ? &*it : nullptr;
}

enum class Source : uint8_t { Data, Code, Branch };

const char* SourceName(Source s) {
  switch (s) {
    case Source::Data:
      return "data pointer";
    case Source::Code:
      return "lis/addi pointer";
    case Source::Branch:
      return "tail branch";
  }
  return "?";
}

struct Candidate {
  Source source;
  uint32_t ref;  // data word address, lis/addi site, or branch site
};

/// True if the register loaded with a lis/addi-built address is used as a load base within
/// the next few instructions (switch/jump tables and constant tables, not code pointers).
bool usedAsLoadBase(const BinaryView& binary, uint32_t site, uint32_t reg) {
  for (uint32_t a = site + 4; a <= site + 4 * 8; a += 4) {
    auto w = readWord(binary, a);
    if (!w)
      return false;
    uint32_t x = *w;
    uint32_t op = x >> 26;
    uint32_t rt = (x >> 21) & 31, ra = (x >> 16) & 31, rb = (x >> 11) & 31;
    // D-form loads: lwz lwzu lbz lbzu lhz lhzu lha lhau lfs lfsu lfd lfdu, DS-form ld/lwa
    bool dLoad = (op >= 32 && op <= 35) || (op >= 40 && op <= 43) || (op >= 48 && op <= 51) ||
                 op == 58;
    if (dLoad && ra == reg)
      return true;
    if (op == 31) {
      uint32_t xo = (x >> 1) & 0x3FF;
      // lwzx lwzux lbzx lbzux lhzx lhzux lhax lhaux ldx lwax lfsx lfdx lvx lvlx lvrx
      static constexpr uint32_t kIndexedLoads[] = {23, 55, 87, 119, 279, 311, 343, 375,
                                                   21, 341, 535, 599, 103, 519, 551};
      if (std::find(std::begin(kIndexedLoads), std::end(kIndexedLoads), xo) !=
              std::end(kIndexedLoads) &&
          (ra == reg || rb == reg))
        return true;
    }
    if (isUncondTerminator(x))
      return false;
    // Register overwritten by a D-form ALU op or a load into it: stop.
    if ((op == 14 || op == 15 || dLoad) && rt == reg)
      return false;
  }
  return false;
}

class PointerScanner {
 public:
  explicit PointerScanner(CodegenContext& ctx) : ctx_(ctx), binary_(ctx.binary()) {
    regions_ = ctx.scan.codeRegions;
    std::sort(regions_.begin(), regions_.end(),
              [](const CodeRegion& a, const CodeRegion& b) { return a.start < b.start; });
  }

  /// One round: collect candidates, filter, register. Returns the number of new functions.
  size_t round(bool requireTerminatorBefore, bool branchesOnly) {
    std::map<uint32_t, Candidate> cands;
    if (!branchesOnly) {
      collectData(cands);
      collectCode(cands);
    }
    collectBranches(cands);

    Coverage coverage(ctx_.graph);
    std::unordered_set<uint32_t> tableData = jumpTableData();
    size_t added = 0;
    for (const auto& [target, cand] : cands) {
      if (!accept(target, cand, coverage, tableData, requireTerminatorBefore))
        continue;
      ctx_.graph.addFunction(target, 4, FunctionAuthority::DISCOVERED, true);
      counts_[static_cast<size_t>(cand.source)]++;
      added++;
      REXCODEGEN_DEBUG("PointerScan: sub_{:08X} from {} at 0x{:08X}", target,
                       SourceName(cand.source), cand.ref);
    }
    return added;
  }

  void logTotals(const char* phase) const {
    REXCODEGEN_INFO("{}: {} functions from data pointers, {} from lis/addi pointers, {} from "
                    "tail branches",
                    phase, counts_[0], counts_[1], counts_[2]);
  }

 private:
  bool accept(uint32_t t, const Candidate& cand, const Coverage& coverage,
              const std::unordered_set<uint32_t>& tableData, bool requireTerminatorBefore) {
    auto& graph = ctx_.graph;
    if (t & 3)
      return false;
    if (graph.isEntryPoint(t) || graph.isImport(t))
      return false;
    if (binary_.isInImportExportRange(t))
      return false;
    const CodeRegion* region = findRegion(regions_, t);
    if (!region)
      return false;
    if (coverage.covers(t))
      return false;
    if (tableData.contains(t))
      return false;
    auto insn = ctx_.decoded().get(t);
    if (!insn || isInvalid(*insn))
      return false;
    if (requireTerminatorBefore && t != region->start) {
      auto prev = readWord(binary_, t - 4);
      if (!prev || !(*prev == 0 || isUncondTerminator(*prev)))
        return false;
    }
    if (cand.source == Source::Code) {
      // lis/addi addresses: drop the ones used as a load base (tables) - re-check at the site
      auto w = readWord(binary_, cand.ref);
      if (w) {
        uint32_t x = *w;
        uint32_t rd = ((x >> 26) == 24) ? ((x >> 16) & 31) : ((x >> 21) & 31);
        if (usedAsLoadBase(binary_, cand.ref, rd))
          return false;
      }
    }
    return true;
  }

  void collectData(std::map<uint32_t, Candidate>& cands) {
    for (const auto& section : binary_.sections()) {
      if (section.executable || !section.data)
        continue;
      for (uint32_t off = 0; off + 4 <= section.size; off += 4) {
        uint32_t v = load_and_swap<uint32_t>(section.data + off);
        if ((v & 3) || !binary_.isExecutable(v))
          continue;
        cands.try_emplace(v, Candidate{Source::Data, section.baseAddress + off});
      }
    }
  }

  void collectCode(std::map<uint32_t, Candidate>& cands) {
    for (const auto& section : binary_.sections()) {
      if (!section.executable || !section.data)
        continue;
      uint32_t lis[32];
      uint32_t valid = 0;
      for (uint32_t off = 0; off + 4 <= section.size; off += 4) {
        uint32_t addr = section.baseAddress + off;
        if (binary_.isInImportExportRange(addr))
          break;
        uint32_t x = load_and_swap<uint32_t>(section.data + off);
        uint32_t op = x >> 26;
        uint32_t rd = (x >> 21) & 31, ra = (x >> 16) & 31;
        uint32_t imm = x & 0xFFFF;
        if (op == 15 && ra == 0) {  // lis rd, imm
          lis[rd] = imm << 16;
          valid |= 1u << rd;
          continue;
        }
        uint32_t v = 0;
        bool hit = false;
        if (op == 14 && ra != 0 && (valid & (1u << ra))) {  // addi rd, ra, simm
          v = lis[ra] + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm)));
          hit = true;
        } else if (op == 24 && (valid & (1u << rd))) {  // ori ra, rs(=rd field), uimm
          v = lis[rd] | imm;
          hit = true;
        }
        if (hit && !(v & 3) && binary_.isExecutable(v)) {
          cands.try_emplace(v, Candidate{Source::Code, addr});
        }
        if (isUncondTerminator(x))
          valid = 0;
      }
    }
  }

  void collectBranches(std::map<uint32_t, Candidate>& cands) {
    for (const auto& [addr, node] : ctx_.graph.functions()) {
      for (const auto& jump : node->unresolvedJumps()) {
        if (jump.isCall)
          continue;
        cands.try_emplace(jump.target, Candidate{Source::Branch, jump.site});
      }
    }
  }

  /// Words of detected jump tables that sit in code (bases right after a bctr).
  std::unordered_set<uint32_t> jumpTableData() const {
    std::unordered_set<uint32_t> out;
    for (const auto& [addr, node] : ctx_.graph.functions()) {
      for (const auto& jt : node->jumpTables()) {
        if (!jt.tableAddress)
          continue;
        size_t n = std::max<size_t>(jt.targets.size(), 1);
        for (size_t i = 0; i < n; i++) {
          out.insert(jt.tableAddress + static_cast<uint32_t>(i * 4));
        }
      }
    }
    return out;
  }

  CodegenContext& ctx_;
  const BinaryView& binary_;
  std::vector<CodeRegion> regions_;
  size_t counts_[3] = {0, 0, 0};
};

/// Discover blocks for newly registered functions (and the bl targets they add) until none
/// is pending.
void discoverToFixedPoint(CodegenContext& ctx) {
  const size_t maxIterations = REXCVAR_GET(max_discovery_iterations);
  for (size_t i = 0; i < maxIterations; i++) {
    auto known = buildKnownFunctions(ctx.graph);
    if (discoverPendingFunctions(ctx, known) == 0)
      break;
  }
}

}  // anonymous namespace

namespace phases {

VoidResult PointerScan(CodegenContext& ctx) {
  if (!REXCVAR_GET(pointer_scan)) {
    REXCODEGEN_INFO("Analyze: pointer scan disabled (--pointer_scan=false)");
    return Ok();
  }
  PointerScanner scanner(ctx);
  size_t total = 0;
  for (int pass = 0; pass < 16; pass++) {
    size_t added = scanner.round(/*requireTerminatorBefore=*/true, /*branchesOnly=*/false);
    if (added == 0)
      break;
    total += added;
    discoverToFixedPoint(ctx);
  }
  scanner.logTotals("Analyze: pointer scan");
  REXCODEGEN_INFO("Analyze: pointer scan registered {} functions ({} total)", total,
                  ctx.graph.functionCount());
  return Ok();
}

VoidResult OrphanBranches(CodegenContext& ctx) {
  if (!REXCVAR_GET(pointer_scan))
    return Ok();
  PointerScanner scanner(ctx);
  size_t total = 0;
  for (int pass = 0; pass < 16; pass++) {
    size_t added = scanner.round(/*requireTerminatorBefore=*/false, /*branchesOnly=*/true);
    if (added == 0)
      break;
    total += added;
    discoverToFixedPoint(ctx);
  }
  if (total > 0) {
    REXCODEGEN_INFO("Analyze: registered {} tail-branch targets that no function covered", total);
  }
  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
