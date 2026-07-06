package conformance.run

# The §8.5 string-tag trigger: `sqlish"""…"""` hands the macro an
# Ast.StringTemplate (post-dedent static chunks + unevaluated interpolant
# Asts). This tag compiles the template into a parameterized query: static
# text stays SQL, every #{…} hole becomes a $N placeholder bound separately —
# the structural injection-safety pattern from the multiline design §7.
string_tag macro def sqlish(t as Ast.StringTemplate):
  query = ""
  params = []
  n = 0
  t.parts.each |part|:
    if part.kind == "AstStringText":
      query = query + part.value
    elif part.kind == "AstStringExpr":
      n = n + 1
      query = query + "$" + n.to_str()
      params.push!(part.expr)
    else:
      raise ValueError.new("unsupported template part: " + part.kind)
  return Ast.node("AstTupleLiteral", {
    elements: [
      Ast.lift(query),
      Ast.node("AstListLiteral", {elements: params})
    ]
  })

def probe():
  flag = true
  q, args = sqlish"""
    SELECT x FROM t
    WHERE a = #{flag} AND b = #{1 + 2}
    """
  ok_query = q == "SELECT x FROM t\nWHERE a = $1 AND b = $2\n"
  ok_args = args == [true, 3]
  if ok_query and ok_args:
    42
  else:
    0
