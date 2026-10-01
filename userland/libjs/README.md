# JavaScript library foundation

R0 provides proposed public headers, a precise runtime contract, and examples
for interface review. R1 source preparation retains the pinned QuickJS core,
manifest, notices, and one scoped patch. There is no production libjs.so or
runner in this slice. The shared libos64 prerequisites have implementation and
host/guest fixtures; the runtime symbols declared here do not.

Read CONTRACT.md before implementing a consumer. Run
`tools/test_js_contract_headers.sh` for syntax checks and source verification,
`tools/test_js_engine_host.sh` for the isolated upstream host probe, and
`tools/test_js_support_host.sh` for the shared-library prerequisites. Actual
os64 runtime execution remains R1/R2/J1 work, alongside Opus's M1 maths port.
