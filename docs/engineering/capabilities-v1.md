# amber.capabilities.v1

Status: implemented for the `W11.1` capability/sandbox baseline.

`amber.toml` supports a restricted `[capabilities]` section. Manifest entries
are deny-by-default requests; host policy grants are supplied separately and the
runtime uses only the resolved requested/granted intersection.

Supported manifest forms:

```toml
[capabilities]
fs.read = ["./data"]
net.connect = ["api.example.com:443"]
time = true
random = true
ffi = false
```

The parser canonicalizes profile aliases such as `time`, `random`, `ffi`,
`gpu`, `process`, and `secrets` into the v20 canonical capability names.
Boolean `false` records no request. Boolean `true` records a wildcard target
request for targetless or alias-wide capabilities.

`.amberbc` now has an optional `CAPS` section carrying capability request
metadata. `package-build` and `image-build` copy package manifest capability
requests into each compiled module so package artifacts, frozen images, and
runtime worlds all observe the same host-resource contract.

The CLI preflight is:

```sh
amberc capabilities-check amber.toml --grant fs.read=./data --grant time.now
```

It emits `amber.capabilities.v1` JSON with requested, granted, effective, and
denied entries. Missing grants produce `CapabilityError`.

Runtime hosts can construct `RuntimeWorld` with `RuntimeWorldOptions` and use
`check_capability(name, target)` before host-resource operations. Denied checks
return `CapabilityError`; path-like targets are prefix scoped so `./data`
allows `./data/orders.csv` but not `./database/orders.csv`.
