# sinnet

`sinnet` is a C++ library for building single-threaded, low-latency applications that work with networking and shared memory.

## Tests

This project uses GoogleTest for unit tests.

Build and run tests with CMake:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

By default, CMake first tries `find_package(GTest CONFIG QUIET)`. If GoogleTest is not available in your environment, it can be fetched automatically by setting `SINNET_FETCH_GTEST=ON` (enabled by default).

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

GoogleTest is used as a test-only dependency under the BSD 3-Clause license. See `THIRD_PARTY_NOTICES.md`.
