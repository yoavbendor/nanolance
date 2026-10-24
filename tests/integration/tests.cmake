# Tests of the integration layer added after the move into tests/integration/ (src/integration/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/integration/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.
