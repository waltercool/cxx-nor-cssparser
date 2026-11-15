# cxx-rust-cssparser

A C++ library for parsing CSS.

Despite the historical name, the library is implemented entirely in C++; no Rust
toolchain is required to build it.

## Getting Started

To build cxx-rust-cssparser you will need a C++23 compiler as well as [CMake]
and [ECM]. Build the project like any other CMake project.

[CMake]: https://www.cmake.org
[ECM]: https://commits.kde.org/extra-cmake-modules

## Overview

The goal of this library is not necessarily to implement a full-fledged
web-compatible CSS parser. Instead, it intends to give enough building blocks
to be able to implement such a parser, but also allows you to deviate from the
specification if needed.
