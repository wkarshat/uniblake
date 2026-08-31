/* uniblake — optional C++ wrapper.
 *
 * Header-only, C++11, no dependencies beyond the C library. Nothing in
 * include/ or src/ requires it; a C++ caller may use the C interface directly.
 *
 * What it adds over the C interface:
 *   - storage and alignment handled by the type, so no aligned_alloc
 *   - errors as exceptions at construction, return codes elsewhere
 *   - a prefix object that cannot be modified after construction, so sharing
 *     it across threads is enforced by the type rather than by convention
 */
#ifndef UNIBLAKE_HPP
#define UNIBLAKE_HPP

#include "uniblake/uniblake.h"
#include "uniblake/prefix.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace uniblake {

class Error : public std::runtime_error {
public:
  Error(ub_status c, const char *what) : std::runtime_error(what), code_(c) {}
  ub_status code() const { return code_; }
private:
  ub_status code_;
};

inline void check(int rc, const char *what) {
  if (rc != UB_OK) throw Error(static_cast<ub_status>(rc), what);
}

/* Parameter block. Chainable setters so a call site reads as a description of
 * the hash rather than a sequence of assignments. */
class Params {
public:
  explicit Params(std::size_t digest_length) { ub_param_init(&p_, digest_length); }
  Params &personal(const void *b, std::size_t n) { return copy(p_.personal, UB_PERSONALBYTES, b, n); }
  Params &personal(const std::string &s)         { return personal(s.data(), s.size()); }
  Params &salt(const void *b, std::size_t n)     { return copy(p_.salt, UB_SALTBYTES, b, n); }
  const ub_param *get() const { return &p_; }
private:
  Params &copy(std::uint8_t *dst, std::size_t cap, const void *b, std::size_t n) {
    if (n > cap) throw Error(UB_E_ARG, "field too long");
    std::memset(dst, 0, cap);
    std::memcpy(dst, b, n);
    return *this;
  }
  ub_param p_;
};

/* Owns aligned storage for one ub_state. Copyable: copying duplicates the
 * hashing computation, matching ub_copy. */
class State {
public:
  State() : mem_(new unsigned char[ub_state_size() + ub_state_align()]) {}
  State(const State &o) : State() { check(ub_copy(get(), o.get()), "copy"); }
  State &operator=(const State &o) { check(ub_copy(get(), o.get()), "copy"); return *this; }

  ub_state *get() {
    std::uintptr_t a = ub_state_align();
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(mem_.get());
    return reinterpret_cast<ub_state *>((p + a - 1) & ~(a - 1));
  }
  const ub_state *get() const { return const_cast<State *>(this)->get(); }

  void init(std::size_t digest_length)       { check(ub_init(get(), digest_length), "init"); }
  void init(const Params &P)                 { check(ub_init_param(get(), P.get()), "init_param"); }
  void init_key(std::size_t digest_length, const void *k, std::size_t kl) {
    check(ub_init_key(get(), digest_length, k, kl), "init_key");
  }
  void update(const void *in, std::size_t n) { check(ub_update(get(), in, n), "update"); }
  void update(const std::string &s)          { update(s.data(), s.size()); }
  void final(void *out, std::size_t cap)     { check(ub_final(get(), out, cap), "final"); }
  std::vector<unsigned char> final(std::size_t n) {
    std::vector<unsigned char> d(n);
    final(d.data(), d.size());
    return d;
  }
private:
  std::unique_ptr<unsigned char[]> mem_;
};

/* A state that has absorbed a shared leading segment. Immutable after
 * construction, so it may be shared across threads by const reference. */
class Prefix {
public:
  Prefix(const Params &P, const void *prefix, std::size_t n,
         std::size_t max_tail = 8) {
    s_.init(P);
    if (n) s_.update(prefix, n);
    check(ub_prefix_check(s_.get(), max_tail), "prefix geometry");
    digest_length_ = P.get()->digest_length;
  }

  std::size_t digest_length() const { return digest_length_; }

  /* One digest with caller-supplied trailing bytes. */
  std::vector<unsigned char> hash(const void *tail, std::size_t n) const {
    std::vector<unsigned char> d(digest_length_);
    check(ub_hash_tail(s_.get(), tail, n, d.data(), d.size()), "hash_tail");
    return d;
  }

  /* `count` digests over consecutive counters, packed. */
  std::vector<unsigned char> hash_n(std::uint64_t first, std::size_t count,
                                    std::size_t tail_width = 4) const {
    std::vector<unsigned char> d(count * digest_length_);
    check(ub_hash_n(s_.get(), tail_width, first, count, 0, 0,
                    d.data(), digest_length_), "hash_n");
    return d;
  }

  /* Bytes [off, off+len) of each digest, packed — for digests read as
     fixed-width fields. */
  std::vector<unsigned char> hash_n_field(std::uint64_t first, std::size_t count,
                                          std::size_t off, std::size_t len,
                                          std::size_t tail_width = 4) const {
    std::vector<unsigned char> d(count * len);
    check(ub_hash_n(s_.get(), tail_width, first, count, off, len,
                    d.data(), len), "hash_n field");
    return d;
  }

  /* Continue streaming from the absorbed prefix, for trailing data too long
     or too fragmented for the one-compression path. */
  State resume() const { return s_; }

private:
  State s_;
  std::size_t digest_length_;
};

/* One-shot. */
inline std::vector<unsigned char>
hash(const void *in, std::size_t n, std::size_t digest_length) {
  std::vector<unsigned char> d(digest_length);
  check(ub_hash(d.data(), d.size(), in, n, nullptr, 0), "hash");
  return d;
}

} // namespace uniblake
#endif
