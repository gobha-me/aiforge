cmake_minimum_required(VERSION 3.28)

project(mismatched_dependency LANGUAGES CXX)

# A fetched project that configures successfully but violates every controlled
# dependency's canonical target contract.
add_library(mismatched_dependency INTERFACE)
