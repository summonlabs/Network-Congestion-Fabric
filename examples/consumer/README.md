# Downstream consumer

An independent CMake project that consumes an **installed** Network Congestion
Fabric through `find_package(ncf 1.0 CONFIG REQUIRED)` and links the exported
`ncf::ncf` target. It uses nothing from the fabric source tree.

```
cmake --install <fabric-build> --config Release --prefix <prefix>
cmake -S . -B build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build --config Release
./build/Release/ncf_consumer
```

Expected output ends with `consumer-result=ok`. The program records a policy,
a two-resource topology and one batch of corroborated congestion evidence, then
prints the resulting state, the interventions that state authorizes and the full
explanation.
