/**
 * @file        tests/unit/kernel/xnet_test.cpp
 * @brief       XNet / XSession model (rex/kernel/xam/xnet.h): XNADDR layout, secure IN_ADDR
 *              mapping, key registry, session ids, XSessionCreate decisions, QoS blocks.
 *              The same checks run under wasm64 in tests/web/xnet_web_test.cpp.
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include <rex/kernel/xam/xnet.h>
#include <rex/system/web/net_relay_guest.h>

namespace xn = rex::kernel::xam::xnet;

namespace {
constexpr uint32_t kRelay3 = 0x0A500003u;  // 10.80.0.3
constexpr uint32_t kRelay7 = 0x0A500007u;  // 10.80.0.7
}  // namespace

TEST_CASE("XNADDR layout is big-endian with a MAC derived from the address", "[xnet]") {
  uint8_t xna[xn::kXnAddrSize];
  std::memset(xna, 0xEE, sizeof(xna));
  xn::StoreXnAddr(xna, kRelay3);
  const uint8_t ina[4] = {10, 80, 0, 3};
  CHECK(std::memcmp(xna, ina, 4) == 0);
  for (int i = 4; i < 10; ++i) CHECK(xna[i] == 0);  // inaOnline, wPortOnline
  const uint8_t mac[6] = {0x02, 0x52, 10, 80, 0, 3};
  CHECK(std::memcmp(xna + 10, mac, 6) == 0);
  for (int i = 16; i < 36; ++i) CHECK(xna[i] == 0);  // abOnline
  CHECK(xn::XnAddrIna(xna) == kRelay3);

  // Same bytes as the relay shim's guest helper (H3 hooks and the SDK agree on peers' XNADDRs).
  uint8_t other[36];
  rex::system::web::net_relay::guest::StoreXnAddr(other, kRelay3);
  CHECK(std::memcmp(xna, other, 36) == 0);

  // Never an all-zero MAC, even for 0.0.0.0.
  xn::StoreXnAddr(xna, 0);
  CHECK(xna[10] == 0x02);
}

TEST_CASE("secure IN_ADDRs clear the first byte and map back", "[xnet]") {
  CHECK(xn::ToSecure(kRelay3) == 0x00500003u);
  CHECK(xn::ToSecure(0x00500003u) == 0x00500003u);
  CHECK(xn::IsSecureAddr(0x00500003u));
  CHECK_FALSE(xn::IsSecureAddr(0));
  CHECK_FALSE(xn::IsSecureAddr(kRelay3));

  xn::AddressBook book;
  book.SetLocal(kRelay3);
  // Unregistered secure address: the local address's first byte.
  CHECK(book.TitleToHost(0x00500007u) == kRelay7);
  // Broadcast and plain addresses pass through.
  CHECK(book.TitleToHost(0xFFFFFFFFu) == 0xFFFFFFFFu);
  CHECK(book.TitleToHost(0x0A5000FFu) == 0x0A5000FFu);
  CHECK(book.TitleToHost(0x7F000001u) == 0x7F000001u);

  // Sources: LAN peers (subnet, self, loopback) by their secure address, others unchanged.
  CHECK(book.SourceToTitle(kRelay7) == 0x00500007u);
  CHECK(book.SourceToTitle(kRelay3) == 0x00500003u);
  CHECK(book.SourceToTitle(0x7F000001u) == 0x00000001u);
  CHECK(book.TitleToHost(0x00000001u) == 0x7F000001u);  // loopback round-trips via the book
  CHECK(book.SourceToTitle(0x08080808u) == 0x08080808u);
  CHECK(book.SourceToTitle(0) == 0);
}

TEST_CASE("XnAddrToInAddr registration remembers the XNADDR and XNKID", "[xnet]") {
  xn::AddressBook book;
  book.SetLocal(0xC0A80105u);  // 192.168.1.5 (native LAN)
  uint8_t peer[36];
  xn::StoreXnAddr(peer, 0xC0A80109u);
  peer[20] = 0x5A;  // something in abOnline the lookup must return as is
  const uint8_t kid[8] = {0x01, 2, 3, 4, 5, 6, 7, 8};
  uint32_t secure = book.Register(peer, kid);
  CHECK(secure == 0x00A80109u);
  CHECK(book.TitleToHost(secure) == 0xC0A80109u);

  uint8_t xna_out[36] = {};
  uint8_t kid_out[8] = {};
  bool has_kid = false;
  REQUIRE(book.Lookup(secure, xna_out, kid_out, &has_kid));
  CHECK(has_kid);
  CHECK(std::memcmp(xna_out, peer, 36) == 0);
  CHECK(std::memcmp(kid_out, kid, 8) == 0);
  // Lookup by the host address works too.
  CHECK(book.Lookup(0xC0A80109u, xna_out, nullptr, nullptr));

  // Without a key id.
  uint8_t peer2[36];
  xn::StoreXnAddr(peer2, 0xC0A8010Au);
  uint32_t secure2 = book.Register(peer2, nullptr);
  REQUIRE(book.Lookup(secure2, xna_out, kid_out, &has_kid));
  CHECK_FALSE(has_kid);

  CHECK(book.Unregister(secure));
  CHECK_FALSE(book.Lookup(secure, xna_out, kid_out, &has_kid));
  CHECK_FALSE(book.Unregister(secure));

  uint8_t empty[36] = {};
  CHECK(book.Register(empty, nullptr) == 0);
}

TEST_CASE("key registry", "[xnet]") {
  xn::KeyRegistry keys;
  uint8_t last[8];
  CHECK_FALSE(keys.Last(last));
  const uint8_t a[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  const uint8_t b[8] = {2, 2, 2, 2, 2, 2, 2, 2};
  const uint8_t key[16] = {};
  CHECK(keys.Register(a, key));
  CHECK_FALSE(keys.Register(a, nullptr));  // replacing
  CHECK(keys.Register(b, key));
  CHECK(keys.size() == 2);
  REQUIRE(keys.Last(last));
  CHECK(std::memcmp(last, b, 8) == 0);
  CHECK(keys.IsRegistered(a));
  CHECK(keys.Unregister(a));
  CHECK_FALSE(keys.IsRegistered(a));
  CHECK_FALSE(keys.Unregister(a));
  CHECK(keys.Last(last));  // kept after unregistering
}

TEST_CASE("key ids, keys and nonces", "[xnet]") {
  xn::Random rng(1234);
  for (int i = 0; i < 64; ++i) {
    uint8_t kid[8];
    xn::MakeXnKid(kid, rng);
    CHECK((kid[0] & 0xF0) == xn::kXnKidSystemLink);
    CHECK_FALSE(xn::AllZero(kid, 8));
    xn::MakeXnKid(kid, rng, xn::kXnKidOnlinePeer);
    CHECK((kid[0] & 0xF0) == 0x80);
    CHECK(xn::MakeNonce(rng) != 0);
  }
  // Two consoles (different seeds) get different ids.
  xn::Random other(99);
  uint8_t k1[8], k2[8];
  xn::MakeXnKid(k1, rng);
  xn::MakeXnKid(k2, other);
  CHECK(std::memcmp(k1, k2, 8) != 0);

  // Deterministic mode: constant bytes, still a valid system-link id.
  uint8_t kid[8];
  xn::MakeXnKid(kid, rng, xn::kXnKidSystemLink, true);
  const uint8_t expect[8] = {0x0B, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
  CHECK(std::memcmp(kid, expect, 8) == 0);
  CHECK(xn::MakeNonce(rng, true) == 0xBBBBBBBBBBBBBBBBull);
}

TEST_CASE("XSessionCreate arguments and decisions", "[xnet]") {
  uint8_t buf[28];
  const uint32_t words[7] = {0x40001234u, 0x21u, 8, 2, 0, 0x8F000000u, 0x8F000040u};
  for (int i = 0; i < 7; ++i) xn::StoreBE32(buf + 4 * i, words[i]);
  xn::SessionCreateArgs args;
  REQUIRE(xn::ParseSessionCreateArgs(buf, 28, &args));
  CHECK(args.session == 0x40001234u);
  CHECK(args.flags == 0x21u);
  CHECK(args.public_slots == 8);
  CHECK(args.private_slots == 2);
  CHECK(args.session_info_ptr == 0x8F000000u);
  CHECK(args.nonce_ptr == 0x8F000040u);
  CHECK(xn::ParseSessionCreateArgs(buf, 0, &args));  // length 0 is allowed (XMsg)
  CHECK_FALSE(xn::ParseSessionCreateArgs(buf, 20, &args));
  CHECK_FALSE(xn::ParseSessionCreateArgs(nullptr, 28, &args));

  using A = xn::SessionCreateAction;
  // System link host: HOST | USES_PEER_NETWORK.
  CHECK(xn::DecideSessionCreate(0x21, false) == A::kHost);
  // System link client: USES_PEER_NETWORK only.
  CHECK(xn::DecideSessionCreate(0x20, false) == A::kJoin);
  CHECK(xn::DecideSessionCreate(0, false) == A::kJoin);
  // Live features without Live are refused, host or not.
  CHECK(xn::DecideSessionCreate(0x23, false) == A::kRefuseLive);   // presence
  CHECK(xn::DecideSessionCreate(0x05, false) == A::kRefuseLive);   // stats
  CHECK(xn::DecideSessionCreate(0x08, false) == A::kRefuseLive);   // matchmaking
  CHECK(xn::DecideSessionCreate(0x31, false) == A::kRefuseLive);   // arbitration
  CHECK(xn::DecideSessionCreate(0x23, true) == A::kHost);
  CHECK(xn::kXOnlineESessionNotLoggedOn == 0x80155209u);
}

TEST_CASE("XSessionCreate fills XSESSION_INFO for a host and keeps a client's", "[xnet]") {
  xn::Random rng(7);
  uint8_t host_xna[36];
  xn::StoreXnAddr(host_xna, kRelay3);

  uint8_t info[xn::kSessionInfoSize] = {};
  uint8_t nonce[8] = {};
  REQUIRE(xn::ApplySessionCreate(xn::SessionCreateAction::kHost, info, nonce, host_xna, rng));
  CHECK_FALSE(xn::AllZero(info, 8));
  CHECK((info[0] & 0xF0) == 0);                       // system-link id
  CHECK(std::memcmp(info + 8, host_xna, 36) == 0);    // hostAddress
  CHECK_FALSE(xn::AllZero(info + 44, 16));            // keyExchangeKey
  CHECK_FALSE(xn::AllZero(nonce, 8));

  // A client passes the host's session in: unchanged.
  uint8_t joined[60];
  std::memcpy(joined, info, 60);
  uint8_t joined_nonce[8];
  std::memcpy(joined_nonce, nonce, 8);
  uint8_t my_xna[36];
  xn::StoreXnAddr(my_xna, kRelay7);
  REQUIRE(xn::ApplySessionCreate(xn::SessionCreateAction::kJoin, joined, joined_nonce, my_xna, rng));
  CHECK(std::memcmp(joined, info, 60) == 0);
  CHECK(std::memcmp(joined_nonce, nonce, 8) == 0);

  // A client that passed nothing gets filled like a host.
  uint8_t blank[60] = {};
  uint8_t blank_nonce[8] = {};
  REQUIRE(xn::ApplySessionCreate(xn::SessionCreateAction::kJoin, blank, blank_nonce, my_xna, rng));
  CHECK_FALSE(xn::AllZero(blank, 8));
  CHECK(std::memcmp(blank + 8, my_xna, 36) == 0);

  // Refused: nothing written. Null pointers are fine.
  uint8_t untouched[60] = {};
  CHECK_FALSE(xn::ApplySessionCreate(xn::SessionCreateAction::kRefuseLive, untouched, nullptr,
                                     host_xna, rng));
  CHECK(xn::AllZero(untouched, 60));
  CHECK(xn::ApplySessionCreate(xn::SessionCreateAction::kHost, nullptr, nullptr, host_xna, rng));
}

TEST_CASE("XNQOS answers every target as contacted", "[xnet]") {
  CHECK(xn::QosBlockSize(0) == 32);
  CHECK(xn::QosBlockSize(3) == 8 + 72);
  uint8_t q[8 + 24 * 3];
  std::memset(q, 0xEE, sizeof(q));
  xn::FillQosAllContacted(q, 3, 4);
  CHECK(xn::LoadBE32(q) == 3);
  CHECK(xn::LoadBE32(q + 4) == 0);
  for (int i = 0; i < 3; ++i) {
    const uint8_t* e = q + 8 + 24 * i;
    CHECK(e[0] == 0x03);  // complete | contacted
    CHECK(xn::LoadBE16(e + 2) == 4);
    CHECK(xn::LoadBE16(e + 4) == 4);
    CHECK(xn::LoadBE16(e + 6) == 0);  // cbData
    CHECK(xn::LoadBE32(e + 8) == 0);  // pbData
    CHECK(xn::LoadBE16(e + 12) == 1);
    CHECK(xn::LoadBE32(e + 16) == 100000000u);
    CHECK(xn::LoadBE32(e + 20) == 100000000u);
  }
}

TEST_CASE("console XUIDs differ from console 2 on", "[xnet]") {
  const uint64_t xuid = 0xE000012345678900ull;
  CHECK(xn::ConsoleXuid(xuid, 0) == xuid);
  CHECK(xn::ConsoleXuid(xuid, 1) == xuid);
  CHECK(xn::ConsoleXuid(xuid, 2) == (xuid ^ 2));
  CHECK(xn::ConsoleXuid(xuid, 3) != xn::ConsoleXuid(xuid, 2));
  CHECK((xn::ConsoleXuid(xuid, 0x12345) >> 16) == (xuid >> 16));
}

TEST_CASE("link and address flag values", "[xnet]") {
  CHECK(xn::kLinkUp == 0x0B);  // ACTIVE | 100MBPS | FULL_DUPLEX
  CHECK((xn::kXnAddrStatic | xn::kXnAddrEthernet) == 0x06);
  CHECK(xn::kConnectConnected == 2);
  CHECK(xn::kSessionLiveFeatures == 0x1E);
}
