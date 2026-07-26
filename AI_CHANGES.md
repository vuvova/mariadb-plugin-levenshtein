# AI_CHANGES.md

## 2026-07-26 — Claude Sonnet 5 (`claude-sonnet-5`)

Follow-up to a code review of `levenshtein.cc` performed in the same session.
Changes:

- **Fixed an uncaught-exception crash in `Item_func_levenshtein_editops::val_str`
  (`LEVENSHTEIN_EDITOPS()`).** The function allocates a full
  `(a.size()+1) x (b.size()+1)` backtracking matrix sized from the two input
  strings. The existing overflow check only guarded against the size
  multiplication wrapping `size_t`; it did not guard against the element
  count exceeding `std::vector<size_t>::max_size()`, which is smaller than
  `SIZE_MAX`. Requesting more elements than `max_size()` makes `std::vector`
  throw `std::length_error` immediately (before even attempting to
  allocate), and the surrounding `try/catch` only caught `std::bad_alloc`,
  so the exception propagated out of the plugin uncaught. Any user able to
  call `LEVENSHTEIN_EDITOPS()` with two sufficiently large strings (roughly
  >1 billion characters combined, achievable with a raised
  `max_allowed_packet`) could crash the server. Added an explicit
  `cell_count > std::vector<size_t>().max_size()` check that reports
  `ER_OUTOFMEMORY` and returns `NULL` instead of constructing an
  oversized vector.

- **Widened exception handling in all four other `try/catch` blocks**
  (`decode_string`, `calculate_limited_distance`,
  `calculate_damerau_distance`, `calculate_weighted_distance`) from
  `catch (const std::bad_alloc &)` to `catch (const std::exception &)`, so
  `std::length_error` (and any other allocation-related exception) is
  handled the same way as `std::bad_alloc` instead of crashing the server.
  These paths only allocate `O(min(len))` memory, so they were not
  practically reachable, but the fix keeps error handling consistent and
  removes the same class of latent bug throughout the file.

- **Removed a dead branch** in `calculate_limited_distance`: the ternary
  `limit == std::numeric_limits<size_t>::max() ? limit : limit + 1` could
  never take its true branch, since `limit` is always derived from a
  non-negative `longlong` (capped at `LLONG_MAX`, well below `SIZE_MAX`).
  Simplified to `limit + 1`.

- Added `#include <exception>` for `std::exception`.

No behavioral change for any documented, in-range input — all existing
`mysql-test/levenshtein` cases are expected to produce identical results.
