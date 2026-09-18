/*
 * AST and parser for the PowerShell subset (see Api.cs).
 *
 * Scannerless recursive descent. PowerShell lexes differently in expression
 * and in argument mode (after a command name); both modes are implemented
 * directly on the character stream. Newlines end statements except inside
 * parentheses. Whatever is not covered raises a ParseException with the line
 * number -- the host then reports the script as failed instead of running
 * half of it.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace System.Management.Automation.Shim
{
    /* ------------------------------------------------------------------ */
    /* AST                                                                 */
    /* ------------------------------------------------------------------ */

    internal abstract class Node { public int Line; }

    internal abstract class Expr : Node { }
    internal abstract class Stmt : Node { }

    internal sealed class ScriptBlock : Node
    {
        public readonly List<Stmt> Statements = new List<Stmt>();
    }

    internal sealed class Literal : Expr { public object Value; }
    internal sealed class VarExpr : Expr { public string Scope; public string Name; }   /* $name, $Env:NAME */
    internal sealed class InterpolatedString : Expr { public readonly List<Expr> Parts = new List<Expr>(); }
    internal sealed class ArrayExpr : Expr { public readonly List<Expr> Items = new List<Expr>(); }
    internal sealed class ParenExpr : Expr { public Pipeline Inner; public bool ForceArray; }
    internal sealed class UnaryExpr : Expr { public string Op; public Expr Operand; }
    internal sealed class BinaryExpr : Expr { public string Op; public Expr Left, Right; }
    internal sealed class MemberExpr : Expr { public Expr Target; public string Name; public List<Expr> Args; public bool IsCall; }
    internal sealed class IndexExpr : Expr { public Expr Target; public Expr Index; }
    internal sealed class ScriptBlockExpr : Expr { public ScriptBlock Body; }

    internal sealed class CmdArg
    {
        public string ParamName;      /* null for a positional value */
        public Expr Value;            /* null for a bare -Switch */
    }

    internal sealed class CommandExpr : Expr
    {
        public Expr Name;             /* Literal string or, after &, any expression */
        public readonly List<CmdArg> Args = new List<CmdArg>();
    }

    internal sealed class Pipeline : Expr
    {
        public readonly List<Expr> Elements = new List<Expr>();
    }

    internal sealed class ExprStmt : Stmt { public Pipeline Value; }
    internal sealed class AssignStmt : Stmt { public VarExpr Target; public string Op; public Pipeline Value; }
    internal sealed class ExitStmt : Stmt { public Pipeline Code; }
    internal sealed class ReturnStmt : Stmt { public Pipeline Value; }
    internal sealed class ThrowStmt : Stmt { public Pipeline Value; }
    internal sealed class IfStmt : Stmt
    {
        public readonly List<Pipeline> Conditions = new List<Pipeline>();
        public readonly List<ScriptBlock> Blocks = new List<ScriptBlock>();
        public ScriptBlock Else;
    }
    internal sealed class WhileStmt : Stmt { public Pipeline Condition; public ScriptBlock Body; }
    internal sealed class TryStmt : Stmt { public ScriptBlock Body; public ScriptBlock Catch; public ScriptBlock Finally; }

    /* ------------------------------------------------------------------ */
    /* Parser                                                              */
    /* ------------------------------------------------------------------ */

    internal sealed class Parser
    {
        private readonly string s;
        private int p;
        private int depth;          /* parenthesis nesting: newlines are whitespace inside */

        private static readonly HashSet<string> comparisonOps = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "eq", "ne", "gt", "ge", "lt", "le", "like", "notlike", "match", "notmatch",
            "contains", "notcontains", "in", "notin", "is", "isnot", "replace", "split", "join"
        };

        public Parser(string text)
        {
            s = (text ?? "").Replace("\r\n", "\n").Replace('\r', '\n');
            if (s.Length > 0 && s[0] == '﻿') s = s.Substring(1);
        }

        /* ---------------- helpers ---------------- */

        private bool AtEnd { get { return p >= s.Length; } }
        private char Cur { get { return p < s.Length ? s[p] : '\0'; } }
        private char At(int i) { return p + i < s.Length ? s[p + i] : '\0'; }

        private int LineOf(int pos)
        {
            int line = 1;
            for (int i = 0; i < pos && i < s.Length; i++) if (s[i] == '\n') line++;
            return line;
        }

        private ParseException Fail(string message)
        {
            return new ParseException("At line:" + LineOf(p) + " char:" + (ColumnOf(p)) + " -- " + message);
        }

        private int ColumnOf(int pos)
        {
            int col = 1;
            for (int i = pos - 1; i >= 0 && s[i] != '\n'; i--) col++;
            return col;
        }

        private static bool IsIdentStart(char c) { return char.IsLetter(c) || c == '_'; }
        private static bool IsIdentChar(char c) { return char.IsLetterOrDigit(c) || c == '_'; }

        /* Spaces, tabs, comments and line continuations. Newlines too when
         * asked for or when inside parentheses. */
        private void SkipTrivia(bool newlines)
        {
            newlines = newlines || depth > 0;
            for (;;)
            {
                char c = Cur;
                if (c == ' ' || c == '\t' || c == '\f' || c == '\v') { p++; continue; }
                if (c == '\n' && newlines) { p++; continue; }
                if (c == '`' && At(1) == '\n') { p += 2; continue; }
                if (c == '`' && At(1) == ' ' && LineEndsAfterSpaces(p + 1)) { p++; continue; }
                if (c == '#') { while (!AtEnd && Cur != '\n') p++; continue; }
                if (c == '<' && At(1) == '#')
                {
                    int e = s.IndexOf("#>", p + 2, StringComparison.Ordinal);
                    if (e < 0) throw Fail("Missing closing '#>' in block comment.");
                    p = e + 2;
                    continue;
                }
                break;
            }
        }

        private bool LineEndsAfterSpaces(int i)
        {
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t')) i++;
            return i >= s.Length || s[i] == '\n';
        }

        private void SkipSpaces() { SkipTrivia(false); }

        private bool AtStatementEnd()
        {
            char c = Cur;
            return AtEnd || c == '\n' || c == ';' || c == '}' || (c == ')' && depth > 0);
        }

        private string PeekWord()
        {
            if (!IsIdentStart(Cur)) return null;
            int q = p;
            while (q < s.Length && IsIdentChar(s[q])) q++;
            /* a keyword is not a command name like exit-foo */
            if (q < s.Length && (s[q] == '-' || s[q] == '.' || s[q] == '\\' || s[q] == ':')) return null;
            return s.Substring(p, q - p);
        }

        private void Expect(char c, string what)
        {
            SkipTrivia(depth > 0);
            if (Cur != c) throw Fail("Missing " + what + " ('" + c + "' expected).");
            p++;
        }

        /* ---------------- statements ---------------- */

        public ScriptBlock ParseScript()
        {
            ScriptBlock b = ParseStatements(false);
            SkipTrivia(true);
            if (!AtEnd) throw Fail("Unexpected token '" + Cur + "' in script.");
            return b;
        }

        private ScriptBlock ParseStatements(bool inBlock)
        {
            ScriptBlock block = new ScriptBlock { Line = LineOf(p) };
            for (;;)
            {
                SkipTrivia(true);
                while (Cur == ';') { p++; SkipTrivia(true); }
                if (AtEnd)
                {
                    if (inBlock) throw Fail("Missing closing '}' in statement block.");
                    break;
                }
                if (Cur == '}')
                {
                    if (inBlock) break;
                    throw Fail("Unexpected token '}' in expression or statement.");
                }
                block.Statements.Add(ParseStatement());
            }
            return block;
        }

        private ScriptBlock ParseBlock()
        {
            SkipTrivia(true);
            if (Cur != '{') throw Fail("Missing statement block ('{' expected).");
            p++;
            int saveDepth = depth; depth = 0;      /* a block resets the newline rule */
            ScriptBlock b = ParseStatements(true);
            depth = saveDepth;
            if (Cur != '}') throw Fail("Missing closing '}' in statement block.");
            p++;
            return b;
        }

        private Pipeline ParseCondition()
        {
            SkipTrivia(true);
            if (Cur != '(') throw Fail("Missing '(' after keyword.");
            p++; depth++;
            SkipTrivia(true);
            Pipeline cond = ParsePipeline();
            SkipTrivia(true);
            if (Cur != ')') throw Fail("Missing closing ')' after condition.");
            p++; depth--;
            return cond;
        }

        private Stmt ParseStatement()
        {
            int line = LineOf(p);
            string kw = PeekWord();
            if (kw != null)
            {
                switch (kw.ToLowerInvariant())
                {
                    case "if": p += kw.Length; return WithLine(ParseIf(), line);
                    case "exit": p += kw.Length; return WithLine(new ExitStmt { Code = ParseOptionalPipeline() }, line);
                    case "return": p += kw.Length; return WithLine(new ReturnStmt { Value = ParseOptionalPipeline() }, line);
                    case "throw": p += kw.Length; return WithLine(new ThrowStmt { Value = ParseOptionalPipeline() }, line);
                    case "while": p += kw.Length; return WithLine(new WhileStmt { Condition = ParseCondition(), Body = ParseBlock() }, line);
                    case "try": p += kw.Length; return WithLine(ParseTry(), line);
                    case "function": case "filter": case "foreach": case "for": case "do": case "switch":
                    case "param": case "class": case "begin": case "process": case "end": case "trap": case "data":
                    case "workflow": case "using": case "break": case "continue":
                        throw Fail("The '" + kw + "' statement is not supported by this PowerShell subset.");
                }
            }
            if (Cur == '$')
            {
                int save = p;
                VarExpr v = ParseVariable();
                SkipSpaces();
                string op = null;
                if (Cur == '=' && At(1) != '=') op = "=";
                else if ((Cur == '+' || Cur == '-' || Cur == '*' || Cur == '/') && At(1) == '=') op = Cur + "=";
                if (op != null)
                {
                    p += op.Length;
                    SkipTrivia(true);
                    if (AtStatementEnd()) throw Fail("Missing expression after '" + op + "' in assignment.");
                    Pipeline value = ParsePipeline();
                    return WithLine(new AssignStmt { Target = v, Op = op, Value = value }, line);
                }
                p = save;
            }
            Pipeline pipe = ParsePipeline();
            return WithLine(new ExprStmt { Value = pipe }, line);
        }

        private static Stmt WithLine(Stmt st, int line) { st.Line = line; return st; }

        private Pipeline ParseOptionalPipeline()
        {
            SkipSpaces();
            if (AtStatementEnd()) return null;
            return ParsePipeline();
        }

        private Stmt ParseIf()
        {
            IfStmt st = new IfStmt();
            st.Conditions.Add(ParseCondition());
            st.Blocks.Add(ParseBlock());
            for (;;)
            {
                int save = p;
                SkipTrivia(true);
                string kw = PeekWord();
                if (kw != null && kw.Equals("elseif", StringComparison.OrdinalIgnoreCase))
                {
                    p += kw.Length;
                    st.Conditions.Add(ParseCondition());
                    st.Blocks.Add(ParseBlock());
                    continue;
                }
                if (kw != null && kw.Equals("else", StringComparison.OrdinalIgnoreCase))
                {
                    p += kw.Length;
                    SkipTrivia(true);
                    string kw2 = PeekWord();
                    if (kw2 != null && kw2.Equals("if", StringComparison.OrdinalIgnoreCase))
                    {
                        p += kw2.Length;
                        st.Conditions.Add(ParseCondition());
                        st.Blocks.Add(ParseBlock());
                        continue;
                    }
                    st.Else = ParseBlock();
                    break;
                }
                p = save;
                break;
            }
            return st;
        }

        private Stmt ParseTry()
        {
            TryStmt st = new TryStmt { Body = ParseBlock() };
            for (;;)
            {
                int save = p;
                SkipTrivia(true);
                string kw = PeekWord();
                if (kw != null && kw.Equals("catch", StringComparison.OrdinalIgnoreCase))
                {
                    p += kw.Length;
                    SkipTrivia(true);
                    if (Cur == '[')
                    {
                        /* typed catch: accept and ignore the type list */
                        while (!AtEnd && Cur != '{') p++;
                    }
                    st.Catch = ParseBlock();
                    continue;
                }
                if (kw != null && kw.Equals("finally", StringComparison.OrdinalIgnoreCase))
                {
                    p += kw.Length;
                    st.Finally = ParseBlock();
                    continue;
                }
                p = save;
                break;
            }
            if (st.Catch == null && st.Finally == null) throw Fail("Missing 'catch' or 'finally' after 'try'.");
            return st;
        }

        /* ---------------- pipelines and commands ---------------- */

        private Pipeline ParsePipeline()
        {
            Pipeline pipe = new Pipeline { Line = LineOf(p) };
            pipe.Elements.Add(ParsePipelineElement());
            for (;;)
            {
                SkipSpaces();
                if (Cur != '|') break;
                p++;
                SkipTrivia(true);
                if (AtEnd) throw Fail("An empty pipe element is not allowed.");
                pipe.Elements.Add(ParsePipelineElement());
            }
            return pipe;
        }

        private bool IsCommandStart()
        {
            char c = Cur;
            if (IsIdentStart(c)) return true;
            if (c == '.' && (At(1) == '\\' || At(1) == '/')) return true;
            if (c == '\\' || c == '/') return true;
            if (c == '.' && IsIdentStart(At(1))) return false;
            return false;
        }

        private Expr ParsePipelineElement()
        {
            SkipSpaces();
            int line = LineOf(p);
            if (Cur == '&')
            {
                p++;
                SkipSpaces();
                Expr name = ParseArgAtom();
                CommandExpr c = ParseCommandArgs(name);
                c.Line = line;
                return c;
            }
            if (IsCommandStart())
            {
                string name = ReadCommandName();
                CommandExpr c = ParseCommandArgs(new Literal { Value = name, Line = line });
                c.Line = line;
                return c;
            }
            Expr e = ParseExpr();
            e.Line = line;
            return e;
        }

        private string ReadCommandName()
        {
            int start = p;
            while (!AtEnd)
            {
                char c = Cur;
                if (char.IsWhiteSpace(c) || c == '(' || c == ')' || c == '{' || c == '}' || c == '|' || c == ';'
                    || c == ',' || c == '"' || c == '\'' || c == '$' || c == '`' || c == '#')
                    break;
                p++;
            }
            return s.Substring(start, p - start);
        }

        private CommandExpr ParseCommandArgs(Expr name)
        {
            CommandExpr cmd = new CommandExpr { Name = name };
            for (;;)
            {
                SkipSpaces();
                if (AtEnd) break;
                char c = Cur;
                if (c == '\n' || c == ';' || c == '|' || c == '}' || c == ')') break;
                if (c == '-' && (IsIdentStart(At(1))))
                {
                    int q = p + 1;
                    while (q < s.Length && IsIdentChar(s[q])) q++;
                    string pname = s.Substring(p + 1, q - p - 1);
                    p = q;
                    Expr val = null;
                    if (Cur == ':' && !char.IsWhiteSpace(At(1)) && At(1) != '\0') { p++; val = ParseArgValue(); }
                    cmd.Args.Add(new CmdArg { ParamName = pname, Value = val });
                    continue;
                }
                if (c == '-' && At(1) == '-' && IsIdentStart(At(2)))
                {
                    /* --long-option of a native command */
                    cmd.Args.Add(new CmdArg { Value = ParseBareword() });
                    continue;
                }
                cmd.Args.Add(new CmdArg { Value = ParseArgValue() });
            }
            return cmd;
        }

        private Expr ParseArgValue()
        {
            Expr first = ParseArgAtom();
            List<Expr> items = null;
            for (;;)
            {
                int save = p;
                SkipSpaces();
                if (Cur == ',')
                {
                    p++;
                    SkipTrivia(true);
                    if (items == null) { items = new List<Expr>(); items.Add(first); }
                    items.Add(ParseArgAtom());
                    continue;
                }
                p = save;
                break;
            }
            if (items == null) return first;
            ArrayExpr a = new ArrayExpr { Line = first.Line };
            a.Items.AddRange(items);
            return a;
        }

        private Expr ParseArgAtom()
        {
            int line = LineOf(p);
            char c = Cur;
            Expr e;
            if (c == '$') e = ParsePostfix(ParseDollar());
            else if (c == '(') e = ParsePostfix(ParseParen(false));
            else if (c == '@' && At(1) == '(') { p++; e = ParsePostfix(ParseParen(true)); }
            else if (c == '@' && At(1) == '{') throw Fail("Hash table literals are not supported by this PowerShell subset.");
            else if (c == '"') e = ParsePostfix(ParseDoubleQuoted());
            else if (c == '\'') e = ParseSingleQuoted();
            else if (c == '{') e = ParseScriptBlockLiteral();
            else if (c == '[') throw Fail("Type literals ([type]) are not supported by this PowerShell subset.");
            else e = ParseBareword();
            e.Line = line;
            return e;
        }

        /* An unquoted argument. Numbers become numbers, everything else is an
         * expandable string ($var and `escapes are honoured, as in PowerShell). */
        private Expr ParseBareword()
        {
            int start = p;
            StringBuilder sb = new StringBuilder();
            InterpolatedString interp = null;
            while (!AtEnd)
            {
                char c = Cur;
                if (char.IsWhiteSpace(c) || c == '|' || c == ';' || c == ')' || c == '}' || c == ',' || c == '(' || c == '"' || c == '\'')
                    break;
                if (c == '`')
                {
                    p++;
                    if (AtEnd) break;
                    sb.Append(Unescape(Cur)); p++;
                    continue;
                }
                if (c == '$' && (IsIdentStart(At(1)) || At(1) == '{' || At(1) == '('))
                {
                    if (interp == null) interp = new InterpolatedString();
                    if (sb.Length > 0) { interp.Parts.Add(new Literal { Value = sb.ToString() }); sb.Length = 0; }
                    interp.Parts.Add(ParseDollar());
                    continue;
                }
                sb.Append(c); p++;
            }
            if (p == start) throw Fail("Unexpected token '" + Cur + "' in expression or statement.");
            if (interp != null)
            {
                if (sb.Length > 0) interp.Parts.Add(new Literal { Value = sb.ToString() });
                return interp;
            }
            string word = sb.ToString();
            object num;
            if (TryParseNumber(word, out num)) return new Literal { Value = num };
            return new Literal { Value = word };
        }

        private static bool TryParseNumber(string word, out object value)
        {
            value = null;
            if (word.Length == 0) return false;
            char c0 = word[0];
            if (!(char.IsDigit(c0) || ((c0 == '-' || c0 == '+' || c0 == '.') && word.Length > 1 && char.IsDigit(word[1])))) return false;
            long l;
            if (word.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                && long.TryParse(word.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out l))
            { value = Values.Normalize(l); return true; }
            if (long.TryParse(word, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out l))
            { value = Values.Normalize(l); return true; }
            double d;
            if (double.TryParse(word, NumberStyles.Float, CultureInfo.InvariantCulture, out d))
            { value = d; return true; }
            return false;
        }

        /* ---------------- expressions ---------------- */

        private Expr ParseExpr()
        {
            return ParseLogical();
        }

        private Expr ParseLogical()
        {
            Expr left = ParseNot();
            for (;;)
            {
                int save = p;
                SkipSpaces();
                string op = PeekOperator();
                if (op == "and" || op == "or" || op == "xor")
                {
                    p += op.Length + 1;
                    SkipTrivia(true);
                    Expr right = ParseNot();
                    left = new BinaryExpr { Op = op, Left = left, Right = right, Line = left.Line };
                    continue;
                }
                p = save;
                break;
            }
            return left;
        }

        private Expr ParseNot()
        {
            SkipSpaces();
            int line = LineOf(p);
            string op = PeekOperator();
            if (op == "not") { p += 4; SkipTrivia(true); return new UnaryExpr { Op = "not", Operand = ParseNot(), Line = line }; }
            if (Cur == '!' && At(1) != '=') { p++; SkipTrivia(true); return new UnaryExpr { Op = "not", Operand = ParseNot(), Line = line }; }
            return ParseComparison();
        }

        private Expr ParseComparison()
        {
            Expr left = ParseAdditive();
            for (;;)
            {
                int save = p;
                SkipSpaces();
                string op = PeekOperator();
                if (op != null)
                {
                    string core = op;
                    bool caseSensitive = false;
                    if (core.Length > 2 && (core[0] == 'i' || core[0] == 'c') && comparisonOps.Contains(core.Substring(1)))
                    {
                        caseSensitive = core[0] == 'c';
                        core = core.Substring(1);
                    }
                    if (comparisonOps.Contains(core))
                    {
                        p += op.Length + 1;
                        SkipTrivia(true);
                        Expr right = ParseAdditive();
                        left = new BinaryExpr { Op = (caseSensitive ? "c" : "") + core.ToLowerInvariant(), Left = left, Right = right, Line = left.Line };
                        continue;
                    }
                }
                p = save;
                break;
            }
            return left;
        }

        private Expr ParseAdditive()
        {
            Expr left = ParseMultiplicative();
            for (;;)
            {
                int save = p;
                SkipSpaces();
                char c = Cur;
                if ((c == '+' || c == '-') && At(1) != '=' && !(c == '-' && IsIdentStart(At(1))))
                {
                    p++;
                    SkipTrivia(true);
                    Expr right = ParseMultiplicative();
                    left = new BinaryExpr { Op = c.ToString(), Left = left, Right = right, Line = left.Line };
                    continue;
                }
                p = save;
                break;
            }
            return left;
        }

        private Expr ParseMultiplicative()
        {
            Expr left = ParseUnary();
            for (;;)
            {
                int save = p;
                SkipSpaces();
                char c = Cur;
                if ((c == '*' || c == '/' || c == '%') && At(1) != '=')
                {
                    p++;
                    SkipTrivia(true);
                    Expr right = ParseUnary();
                    left = new BinaryExpr { Op = c.ToString(), Left = left, Right = right, Line = left.Line };
                    continue;
                }
                p = save;
                break;
            }
            return left;
        }

        private Expr ParseUnary()
        {
            SkipSpaces();
            int line = LineOf(p);
            if (Cur == '-' && !IsIdentStart(At(1)))
            {
                p++;
                SkipSpaces();
                return new UnaryExpr { Op = "-", Operand = ParseUnary(), Line = line };
            }
            if (Cur == '+' && !char.IsDigit(At(1))) { p++; return ParseUnary(); }
            return ParsePostfix(ParsePrimary());
        }

        /* "-word" at the current position: returns word (lower case) or null. */
        private string PeekOperator()
        {
            if (Cur != '-' || !char.IsLetter(At(1))) return null;
            int q = p + 1;
            while (q < s.Length && char.IsLetter(s[q])) q++;
            if (q < s.Length && (IsIdentChar(s[q]) || s[q] == '-')) return null;
            return s.Substring(p + 1, q - p - 1).ToLowerInvariant();
        }

        private Expr ParsePrimary()
        {
            SkipSpaces();
            int line = LineOf(p);
            char c = Cur;
            Expr e;
            if (c == '$') e = ParseDollar();
            else if (c == '(') e = ParseParen(false);
            else if (c == '@' && At(1) == '(') { p++; e = ParseParen(true); }
            else if (c == '@' && At(1) == '{') throw Fail("Hash table literals are not supported by this PowerShell subset.");
            else if (c == '"') e = ParseDoubleQuoted();
            else if (c == '\'') e = ParseSingleQuoted();
            else if (c == '{') e = ParseScriptBlockLiteral();
            else if (c == '[') throw Fail("Type literals ([type]) are not supported by this PowerShell subset.");
            else if (char.IsDigit(c) || (c == '.' && char.IsDigit(At(1)))) e = ParseNumber();
            else if (AtEnd) throw Fail("Missing expression.");
            else throw Fail("Unexpected token '" + c + "' in expression or statement.");
            e.Line = line;
            return e;
        }

        private Expr ParseNumber()
        {
            int start = p;
            while (!AtEnd && (char.IsLetterOrDigit(Cur) || Cur == '.')) p++;
            string word = s.Substring(start, p - start);
            object num;
            if (!TryParseNumber(word, out num)) throw Fail("Invalid numeric constant '" + word + "'.");
            return new Literal { Value = num };
        }

        private Expr ParsePostfix(Expr target)
        {
            for (;;)
            {
                if (Cur == '.' && (IsIdentStart(At(1))))
                {
                    p++;
                    int start = p;
                    while (!AtEnd && IsIdentChar(Cur)) p++;
                    MemberExpr m = new MemberExpr { Target = target, Name = s.Substring(start, p - start), Line = target.Line };
                    if (Cur == '(')
                    {
                        p++; depth++;
                        m.IsCall = true;
                        m.Args = new List<Expr>();
                        SkipTrivia(true);
                        if (Cur != ')')
                        {
                            for (;;)
                            {
                                m.Args.Add(ParseExpr());
                                SkipTrivia(true);
                                if (Cur == ',') { p++; SkipTrivia(true); continue; }
                                break;
                            }
                        }
                        if (Cur != ')') throw Fail("Missing ')' in method call.");
                        p++; depth--;
                    }
                    target = m;
                    continue;
                }
                if (Cur == '[' )
                {
                    p++; depth++;
                    SkipTrivia(true);
                    Expr idx = ParseExpr();
                    SkipTrivia(true);
                    if (Cur != ']') throw Fail("Missing ']' after array index expression.");
                    p++; depth--;
                    target = new IndexExpr { Target = target, Index = idx, Line = target.Line };
                    continue;
                }
                break;
            }
            return target;
        }

        /* ( pipeline ) or @( pipeline ) -- the cursor is on '('. */
        private Expr ParseParen(bool forceArray)
        {
            int line = LineOf(p);
            p++; depth++;
            SkipTrivia(true);
            ParenExpr pe = new ParenExpr { ForceArray = forceArray, Line = line };
            if (Cur == ')') pe.Inner = null;
            else
            {
                pe.Inner = ParsePipeline();
                SkipTrivia(true);
                if (Cur == ';') { throw Fail("Multiple statements inside parentheses are not supported by this PowerShell subset."); }
            }
            if (Cur != ')') throw Fail("Missing closing ')' in expression.");
            p++; depth--;
            return pe;
        }

        private Expr ParseScriptBlockLiteral()
        {
            int line = LineOf(p);
            ScriptBlock body = ParseBlock();
            return new ScriptBlockExpr { Body = body, Line = line };
        }

        /* $name, $scope:name, ${name}, $(pipeline), $?, $_, $$ -- cursor on '$'. */
        private Expr ParseDollar()
        {
            int line = LineOf(p);
            if (At(1) == '(')
            {
                p++;
                Expr sub = ParseParen(false);
                sub.Line = line;
                return sub;
            }
            VarExpr v = ParseVariable();
            v.Line = line;
            return v;
        }

        private VarExpr ParseVariable()
        {
            if (Cur != '$') throw Fail("Variable expected.");
            p++;
            if (Cur == '{')
            {
                int e = s.IndexOf('}', p);
                if (e < 0) throw Fail("Missing closing '}' in variable name.");
                string full = s.Substring(p + 1, e - p - 1);
                p = e + 1;
                return SplitScope(full);
            }
            if (Cur == '?' || Cur == '_' && !IsIdentChar(At(1)) || Cur == '$' || Cur == '^')
            {
                string special = Cur.ToString(); p++;
                return new VarExpr { Name = special };
            }
            int start = p;
            while (!AtEnd && IsIdentChar(Cur)) p++;
            if (p == start) throw Fail("Missing variable name after '$'.");
            if (Cur == ':' && IsIdentStart(At(1)))
            {
                int q = p + 1;
                while (q < s.Length && IsIdentChar(s[q])) q++;
                string scope = s.Substring(start, p - start);
                string name = s.Substring(p + 1, q - p - 1);
                p = q;
                return new VarExpr { Scope = scope, Name = name };
            }
            return new VarExpr { Name = s.Substring(start, p - start) };
        }

        private static VarExpr SplitScope(string full)
        {
            int colon = full.IndexOf(':');
            if (colon > 0) return new VarExpr { Scope = full.Substring(0, colon), Name = full.Substring(colon + 1) };
            return new VarExpr { Name = full };
        }

        private Expr ParseSingleQuoted()
        {
            p++;
            StringBuilder sb = new StringBuilder();
            for (;;)
            {
                if (AtEnd) throw Fail("The string is missing the terminator: '.");
                char c = Cur;
                if (c == '\'')
                {
                    if (At(1) == '\'') { sb.Append('\''); p += 2; continue; }
                    p++;
                    break;
                }
                sb.Append(c); p++;
            }
            return new Literal { Value = sb.ToString() };
        }

        private static char Unescape(char c)
        {
            switch (c)
            {
                case 'n': return '\n';
                case 't': return '\t';
                case 'r': return '\r';
                case '0': return '\0';
                case 'a': return '\a';
                case 'b': return '\b';
                case 'f': return '\f';
                case 'v': return '\v';
                default: return c;
            }
        }

        /* "text $var $Env:X ${name} $(expr) `n" -- cursor on the opening quote. */
        private Expr ParseDoubleQuoted()
        {
            p++;
            InterpolatedString str = new InterpolatedString();
            StringBuilder sb = new StringBuilder();
            for (;;)
            {
                if (AtEnd) throw Fail("The string is missing the terminator: \".");
                char c = Cur;
                if (c == '"')
                {
                    if (At(1) == '"') { sb.Append('"'); p += 2; continue; }
                    p++;
                    break;
                }
                if (c == '`')
                {
                    p++;
                    if (AtEnd) throw Fail("The string is missing the terminator: \".");
                    sb.Append(Unescape(Cur)); p++;
                    continue;
                }
                if (c == '$' && (IsIdentStart(At(1)) || At(1) == '{' || At(1) == '(' || At(1) == '?' || At(1) == '_'))
                {
                    if (sb.Length > 0) { str.Parts.Add(new Literal { Value = sb.ToString() }); sb.Length = 0; }
                    int saveDepth = depth; depth = 0;
                    str.Parts.Add(ParseDollar());
                    depth = saveDepth;
                    continue;
                }
                sb.Append(c); p++;
            }
            if (sb.Length > 0 || str.Parts.Count == 0) str.Parts.Add(new Literal { Value = sb.ToString() });
            if (str.Parts.Count == 1 && str.Parts[0] is Literal) return str.Parts[0];
            return str;
        }
    }
}
