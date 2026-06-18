# Url in Amber

`Url` is a native standard-library module for RFC-style URL parsing, building,
percent coding, and query string maps.

## Parse And Build

```amber
u = Url.parse("https://user@example.com:8443/api?q=amber#top")

u["scheme"]   # "https"
u["host"]     # "example.com"
u["port"]     # 8443
u["path"]     # "/api"
u["query"]    # "q=amber"
u["fragment"] # "top"

Url.build(u)  # "https://user@example.com:8443/api?q=amber#top"
```

`Url.parse` returns a Map with these keys:

| Key | Value |
| --- | --- |
| `scheme` | Str or `null` |
| `authority` | raw authority Str or `null` |
| `userinfo` | Str or `null` |
| `host` | lowercased host Str or `null` |
| `port` | Int or `null` |
| `path` | Str |
| `query` | raw query Str or `null` |
| `fragment` | Str or `null` |
| `query_map` | nested Map/List/Str query values or `null` |

`Url.build(parts)` accepts the same Map shape. When `query` is absent, it can
build the query string from `query_map`.

## Percent Coding

```amber
Url.percent_encode("a b/!")       # "a%20b%2F%21"
Url.percent_decode("a%20b%2F%21") # "a b/!"
```

Invalid percent escapes raise `UrlDecodeError`.

## Query Maps

```amber
query = Url.build_query({"q": ["a b", "x/y"], "empty": ""})
# "q[]=a+b&q[]=x%2Fy&empty="

params = Url.parse_query(query)
params["q"][0] # "a b"
params["q"][1] # "x/y"

nested = Url.parse_query("?map[a]=1&map[b][x]=2&c[]=1&c[]=2")
nested["map"]["a"]      # "1"
nested["map"]["b"]["x"] # "2"
nested["c"][0]          # "1"
```

`Url.parse_query` returns a Str for a single scalar key, a List for repeated
keys or `[]` array notation, and a Map for named bracket notation.
`Url.build_query` accepts nested Str, List, and Map values; Lists are emitted
with `[]` notation.
