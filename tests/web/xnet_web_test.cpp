/**
 * @file        tests/web/xnet_web_test.cpp
 * @brief       wasm64 run of the XNet / XSession model checks (rex/kernel/xam/xnet.h), the code
 *              the web NetDll exports use to talk to the net relay: guest byte order, the
 *              secure IN_ADDR mapping on the relay subnet 10.80.0.0/24, XSESSION_INFO, XNQOS.
 *              Native coverage: tests/unit/kernel/xnet_test.cpp. Run: run_xnet_web_test.bat.
 * @license     BSD 3-Clause License
 */

#include <cstdio>
#include <cstring>

#include <rex/kernel/xam/xnet.h>
#include <rex/system/web/net_relay.h>
#include <rex/system/web/net_relay_guest.h>

namespace xn = rex::kernel::xam::xnet;
namespace nr = rex::system::web::net_relay;
namespace ng = rex::system::web::net_relay::guest;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

static void TestXnAddr() {
  uint8_t a[36], b[36];
  xn::StoreXnAddr(a, nr::kSubnet | 3);
  ng::StoreXnAddr(b, nr::kSubnet | 3);
  CHECK(std::memcmp(a, b, 36) == 0);
  CHECK(a[0] == 10 && a[1] == 80 && a[2] == 0 && a[3] == 3);
  CHECK(xn::XnAddrIna(a) == ng::LoadXnAddrIna(b));
  CHECK(a[10] == 0x02 && a[15] == 3);
}

static void TestSecureMappingOnRelaySubnet() {
  xn::AddressBook book;
  book.SetLocal(nr::kSubnet | 3, nr::kSubnetMask);
  // What a peer's XNADDR becomes for the title, and what sendto() maps back to.
  uint8_t peer[36];
  xn::StoreXnAddr(peer, nr::kSubnet | 7);
  uint8_t kid[8] = {0x01, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  uint32_t secure = book.Register(peer, kid);
  CHECK(secure == 0x00500007u);
  CHECK(book.TitleToHost(secure) == (nr::kSubnet | 7));
  // recvfrom: relay peers and self by secure address; broadcast destinations untouched.
  CHECK(book.SourceToTitle(nr::kSubnet | 9) == 0x00500009u);
  CHECK(book.SourceToTitle(nr::kSubnet | 3) == 0x00500003u);
  CHECK(book.TitleToHost(nr::kSubnetBroadcast) == nr::kSubnetBroadcast);
  CHECK(book.TitleToHost(nr::kLimitedBroadcast) == nr::kLimitedBroadcast);
  uint8_t out[36], kid_out[8];
  bool has_kid = false;
  CHECK(book.Lookup(secure, out, kid_out, &has_kid) && has_kid);
  CHECK(std::memcmp(kid_out, kid, 8) == 0);
}

static void TestGuestSockaddr() {
  // sendto(0.80.0.7:1000) as the guest lays it out.
  uint8_t sa[16] = {};
  ng::StoreSockAddrIn(sa, 16, {ng::kAfInet, 1000, 0x00500007u});
  CHECK(sa[0] == 0 && sa[1] == 2 && sa[2] == 0x03 && sa[3] == 0xE8);
  ng::SockAddrIn parsed{};
  CHECK(ng::LoadSockAddrIn(sa, 16, &parsed));
  xn::AddressBook book;
  book.SetLocal(nr::kSubnet | 3, nr::kSubnetMask);
  CHECK(book.TitleToHost(parsed.addr) == (nr::kSubnet | 7));
  CHECK(parsed.port == 1000);
}

static void TestSession() {
  xn::Random rng(42);
  uint8_t host[36];
  xn::StoreXnAddr(host, nr::kSubnet | 3);
  uint8_t info[60] = {};
  uint8_t nonce[8] = {};
  CHECK(xn::ApplySessionCreate(xn::DecideSessionCreate(0x21, false), info, nonce, host, rng));
  CHECK((info[0] & 0xF0) == 0 && !xn::AllZero(info, 8));
  CHECK(std::memcmp(info + 8, host, 36) == 0);
  CHECK(!xn::AllZero(nonce, 8));
  CHECK(xn::DecideSessionCreate(0x23, false) == xn::SessionCreateAction::kRefuseLive);
  CHECK(xn::DecideSessionCreate(0x20, false) == xn::SessionCreateAction::kJoin);
}

static void TestQos() {
  uint8_t q[8 + 24 * 2];
  xn::FillQosAllContacted(q, 2, 1);
  CHECK(xn::LoadBE32(q) == 2);
  CHECK(q[8] == 3 && q[32] == 3);
  CHECK(xn::LoadBE32(q + 8 + 16) == 100000000u);
}

int main() {
  TestXnAddr();
  TestSecureMappingOnRelaySubnet();
  TestGuestSockaddr();
  TestSession();
  TestQos();
  std::printf("xnet_web_test: %d checks, %d failures (sizeof(void*) = %zu)\n", g_checks,
              g_failures, sizeof(void*));
  return g_failures ? 1 : 0;
}
