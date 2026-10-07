/*
 * Evaluator for the PowerShell subset (see Api.cs).
 *
 * One Interpreter runs one AddScript()/AddCommand() entry and writes its
 * records into the PSDataStreams of the owning PowerShell object. Terminating
 * errors surface as RuntimeException to the host (as the real API does);
 * non-terminating ones become ErrorRecords and the script continues.
 */

using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;

namespace System.Management.Automation.Shim
{
    internal sealed class ExitException : Exception
    {
        public readonly int Code;
        public ExitException(int code) : base("exit " + code) { Code = code; }
    }

    internal sealed class ReturnException : Exception
    {
        public readonly List<object> Value;
        public ReturnException(List<object> value) { Value = value; }
    }

    /* A cmdlet invocation after parameter binding. */
    internal sealed class Invocation
    {
        public string Name;
        public Interpreter Interp;
        public Dictionary<string, object> Bound = new Dictionary<string, object>(StringComparer.OrdinalIgnoreCase);
        public List<object> Input;                 /* pipeline input, may be null */
        public string ErrorAction;                 /* -ErrorAction if given */
        public int Line;

        public bool Has(string name) { return Bound.ContainsKey(name); }
        public object Get(string name) { object v; return Bound.TryGetValue(name, out v) ? v : null; }
        public string GetString(string name) { return Values.ToPSString(Get(name)); }
        public bool GetSwitch(string name) { object v; return Bound.TryGetValue(name, out v) && Values.ToBool(v); }
        public List<object> GetList(string name) { return Values.ToList(Get(name)); }
    }

    internal sealed class Interpreter
    {
        private readonly PSDataStreams streams;
        private readonly Dictionary<string, object> vars = new Dictionary<string, object>(StringComparer.OrdinalIgnoreCase);
        private readonly List<object> output = new List<object>();

        public int ExitCode { get; private set; }
        public bool Exited { get; private set; }
        public PSDataStreams Streams { get { return streams; } }

        public Interpreter(PSDataStreams streams)
        {
            this.streams = streams;
            vars["true"] = true;
            vars["false"] = false;
            vars["null"] = null;
            vars["?"] = true;
            vars["LASTEXITCODE"] = null;
            vars["ErrorActionPreference"] = "Continue";
            vars["WarningPreference"] = "Continue";
            vars["InformationPreference"] = "SilentlyContinue";
            vars["VerbosePreference"] = "SilentlyContinue";
            vars["DebugPreference"] = "SilentlyContinue";
            vars["ProgressPreference"] = "Continue";
            vars["ConfirmPreference"] = "High";
            vars["WhatIfPreference"] = false;
            vars["PSScriptRoot"] = "";
            vars["PSCommandPath"] = "";
            vars["HOME"] = Environment.GetEnvironmentVariable("USERPROFILE") ?? "";
            vars["PWD"] = Directory.GetCurrentDirectory();
            vars["PID"] = System.Diagnostics.Process.GetCurrentProcess().Id;
            vars["PSVersionTable"] = new Dictionary<string, object>(StringComparer.OrdinalIgnoreCase)
            {
                { "PSVersion", new Version(5, 1, 19041, 1) },
                { "PSEdition", "Desktop" },
                { "CLRVersion", Environment.Version },
                { "WineShim", "mono-sma-shim 0.1" }
            };
            vars["Error"] = new ArrayList();
        }

        /* ---------------- entry points ---------------- */

        public List<object> RunScript(string text, List<object> positional, List<KeyValuePair<string, object>> named, List<object> input)
        {
            vars["args"] = positional != null ? positional.ToArray() : new object[0];
            foreach (KeyValuePair<string, object> kv in named) vars[kv.Key] = kv.Value;
            vars["input"] = input != null ? input.ToArray() : new object[0];

            Trace.Log("script: " + text.Length + " chars, " + (positional == null ? 0 : positional.Count) + " positional args");
            ScriptBlock block = new Parser(text).ParseScript();
            Trace.Log("script: parsed, " + block.Statements.Count + " top-level statements");
            try
            {
                ExecBlock(block);
            }
            catch (ExitException ex)
            {
                ExitCode = ex.Code;
                Exited = true;
                Trace.Log("script: exit " + ex.Code);
            }
            catch (ReturnException ex)
            {
                if (ex.Value != null) output.AddRange(ex.Value);
            }
            vars["LASTEXITCODE"] = ExitCode;
            return output;
        }

        public List<object> RunHostCommand(string name, List<object> positional, List<KeyValuePair<string, object>> named, List<object> input)
        {
            CommandExpr cmd = new CommandExpr { Name = new Literal { Value = name } };
            foreach (object o in positional) cmd.Args.Add(new CmdArg { Value = new Literal { Value = o } });
            foreach (KeyValuePair<string, object> kv in named)
                cmd.Args.Add(new CmdArg { ParamName = kv.Key, Value = kv.Value is bool && (bool)kv.Value ? null : new Literal { Value = kv.Value } });
            try
            {
                output.AddRange(RunCommand(cmd, input));
            }
            catch (ExitException ex) { ExitCode = ex.Code; Exited = true; }
            return output;
        }

        /* ---------------- statements ---------------- */

        private void ExecBlock(ScriptBlock block)
        {
            foreach (Stmt st in block.Statements) Exec(st);
        }

        private void Exec(Stmt st)
        {
            ExprStmt es = st as ExprStmt;
            if (es != null)
            {
                List<object> result = RunPipeline(es.Value);
                if (result != null) output.AddRange(result);
                return;
            }
            AssignStmt asg = st as AssignStmt;
            if (asg != null)
            {
                object value = Values.FromList(RunPipeline(asg.Value));
                if (asg.Op != "=")
                {
                    object old = GetVariable(asg.Target);
                    value = Arithmetic(asg.Op.Substring(0, 1), old, value);
                }
                SetVariable(asg.Target, value);
                Trace.Log("assign $" + (asg.Target.Scope != null ? asg.Target.Scope + ":" : "") + asg.Target.Name + " = " + Describe(value));
                return;
            }
            IfStmt ifs = st as IfStmt;
            if (ifs != null)
            {
                for (int i = 0; i < ifs.Conditions.Count; i++)
                {
                    bool cond = Values.ToBool(Values.FromList(RunPipeline(ifs.Conditions[i])));
                    Trace.Log("if (line " + ifs.Line + ") branch " + i + " -> " + cond);
                    if (cond) { ExecBlock(ifs.Blocks[i]); return; }
                }
                if (ifs.Else != null) ExecBlock(ifs.Else);
                return;
            }
            ExitStmt ex = st as ExitStmt;
            if (ex != null)
            {
                int code = 0;
                if (ex.Code != null)
                {
                    object v = Values.FromList(RunPipeline(ex.Code));
                    code = Values.ToInt(v, "exit code");
                }
                throw new ExitException(code);
            }
            ReturnStmt ret = st as ReturnStmt;
            if (ret != null) throw new ReturnException(ret.Value == null ? null : RunPipeline(ret.Value));
            ThrowStmt thr = st as ThrowStmt;
            if (thr != null)
            {
                object v = thr.Value == null ? "ScriptHalted" : Values.FromList(RunPipeline(thr.Value));
                Exception inner = v as Exception;
                RuntimeException rex = new RuntimeException(inner != null ? inner.Message : Values.ToPSString(v), inner);
                rex.ErrorRecord = new ErrorRecord(inner ?? rex, Values.ToPSString(v), ErrorCategory.OperationStopped, v);
                throw rex;
            }
            WhileStmt wh = st as WhileStmt;
            if (wh != null)
            {
                int guard = 0;
                while (Values.ToBool(Values.FromList(RunPipeline(wh.Condition))))
                {
                    ExecBlock(wh.Body);
                    if (++guard > 10000000) throw new RuntimeException("while loop did not terminate.");
                }
                return;
            }
            TryStmt tr = st as TryStmt;
            if (tr != null)
            {
                try
                {
                    ExecBlock(tr.Body);
                }
                catch (ExitException) { throw; }
                catch (ReturnException) { throw; }
                catch (Exception e)
                {
                    if (tr.Catch == null) throw;
                    RuntimeException rex = e as RuntimeException;
                    ErrorRecord rec = rex != null && rex.ErrorRecord != null ? rex.ErrorRecord
                        : new ErrorRecord(e, e.GetType().FullName, ErrorCategory.NotSpecified, null);
                    vars["_"] = rec;
                    ((ArrayList)vars["Error"]).Insert(0, rec);
                    ExecBlock(tr.Catch);
                }
                finally
                {
                    if (tr.Finally != null) ExecBlock(tr.Finally);
                }
                return;
            }
            throw new RuntimeException("Internal error: unknown statement type " + st.GetType().Name);
        }

        /* ---------------- pipelines ---------------- */

        private List<object> RunPipeline(Pipeline pipe)
        {
            List<object> data = null;
            for (int i = 0; i < pipe.Elements.Count; i++)
            {
                Expr e = pipe.Elements[i];
                CommandExpr cmd = e as CommandExpr;
                if (cmd != null)
                {
                    data = RunCommand(cmd, data);
                    continue;
                }
                if (i > 0) throw new RuntimeException("Expressions are only allowed as the first element of a pipeline (line " + e.Line + ").");
                /* Evaluate exactly once: expressions may have side effects
                 * ((Start-Process ...).ExitCode). Arrays enumerate into the
                 * pipeline, everything else is a single item. */
                data = Values.ToList(Eval(e));
            }
            return data ?? new List<object>();
        }

        private List<object> RunCommand(CommandExpr cmd, List<object> input)
        {
            string name = Values.ToPSString(Eval(cmd.Name));
            Cmdlet cmdlet = Cmdlets.Find(name);
            if (cmdlet == null)
            {
                return RunNative(name, cmd, input);
            }

            Invocation inv = new Invocation { Name = cmdlet.Name, Interp = this, Input = input, Line = cmd.Line };
            Bind(cmdlet, cmd, inv);
            if (Trace.Enabled) Trace.Log("cmdlet " + cmdlet.Name + " " + DescribeBound(inv));
            vars["?"] = true;
            List<object> result = cmdlet.Run(inv) ?? new List<object>();
            return result;
        }

        private void Bind(Cmdlet cmdlet, CommandExpr cmd, Invocation inv)
        {
            int nextPositional = 0;
            for (int i = 0; i < cmd.Args.Count; i++)
            {
                CmdArg a = cmd.Args[i];
                if (a.ParamName != null)
                {
                    string pname = a.ParamName;
                    if (IsCommonParameter(pname, ref pname))
                    {
                        if (IsCommonSwitch(pname)) { inv.Bound[pname] = true; continue; }
                        object cv;
                        if (a.Value != null) cv = Eval(a.Value);
                        else if (i + 1 < cmd.Args.Count && cmd.Args[i + 1].ParamName == null) { cv = Eval(cmd.Args[++i].Value); }
                        else throw new ParameterBindingException("Missing an argument for parameter '" + pname + "'.");
                        inv.Bound[pname] = cv;
                        if (pname.Equals("ErrorAction", StringComparison.OrdinalIgnoreCase)) inv.ErrorAction = Values.ToPSString(cv);
                        continue;
                    }
                    CmdletParam par = cmdlet.Resolve(pname);
                    if (par == null)
                        throw new ParameterBindingException(cmdlet.Name + " : A parameter cannot be found that matches parameter name '" + pname + "'.");
                    if (par.IsSwitch)
                    {
                        inv.Bound[par.Name] = a.Value == null ? true : Values.ToBool(Eval(a.Value));
                        continue;
                    }
                    object v;
                    if (a.Value != null) v = Eval(a.Value);
                    else if (i + 1 < cmd.Args.Count && cmd.Args[i + 1].ParamName == null) v = Eval(cmd.Args[++i].Value);
                    else throw new ParameterBindingException(cmdlet.Name + " : Missing an argument for parameter '" + par.Name + "'. Specify a parameter of type '" + par.TypeName + "' and try again.");
                    inv.Bound[par.Name] = v;
                    continue;
                }
                CmdletParam pos = cmdlet.Positional(nextPositional++);
                if (pos == null)
                {
                    if (cmdlet.CollectsRemaining != null)
                    {
                        List<object> rest = Values.ToList(inv.Get(cmdlet.CollectsRemaining));
                        rest.AddRange(Values.ToList(Eval(a.Value)));
                        inv.Bound[cmdlet.CollectsRemaining] = rest.ToArray();
                        continue;
                    }
                    throw new ParameterBindingException(cmdlet.Name + " : A positional parameter cannot be found that accepts argument '" + Values.ToPSString(Eval(a.Value)) + "'.");
                }
                inv.Bound[pos.Name] = Eval(a.Value);
            }
            foreach (CmdletParam par in cmdlet.Params)
                if (par.Mandatory && !inv.Bound.ContainsKey(par.Name))
                    throw new ParameterBindingException(cmdlet.Name + " : Cannot process command because of one or more missing mandatory parameters: " + par.Name + ".");
        }

        private static readonly string[] commonValueParams =
        {
            "ErrorAction", "WarningAction", "InformationAction", "ErrorVariable", "WarningVariable",
            "InformationVariable", "OutVariable", "OutBuffer", "PipelineVariable"
        };
        private static readonly string[] commonSwitchParams = { "Verbose", "Debug", "WhatIf", "Confirm" };

        private static bool IsCommonParameter(string given, ref string canonical)
        {
            foreach (string c in commonValueParams)
                if (c.StartsWith(given, StringComparison.OrdinalIgnoreCase) && given.Length >= 3) { canonical = c; return true; }
            foreach (string c in commonSwitchParams)
                if (c.Equals(given, StringComparison.OrdinalIgnoreCase)) { canonical = c; return true; }
            if (given.Equals("ea", StringComparison.OrdinalIgnoreCase)) { canonical = "ErrorAction"; return true; }
            if (given.Equals("wa", StringComparison.OrdinalIgnoreCase)) { canonical = "WarningAction"; return true; }
            if (given.Equals("ev", StringComparison.OrdinalIgnoreCase)) { canonical = "ErrorVariable"; return true; }
            if (given.Equals("ov", StringComparison.OrdinalIgnoreCase)) { canonical = "OutVariable"; return true; }
            return false;
        }

        private static bool IsCommonSwitch(string canonical)
        {
            return Array.IndexOf(commonSwitchParams, canonical) >= 0;
        }

        /* ---------------- native commands ---------------- */

        private List<object> RunNative(string name, CommandExpr cmd, List<object> input)
        {
            string exe = ResolveExecutable(name);
            if (exe == null)
            {
                CommandNotFoundException cnf = new CommandNotFoundException(
                    "The term '" + name + "' is not recognized as the name of a cmdlet, function, script file, or operable program. "
                    + "Check the spelling of the name, or if a path was included, verify that the path is correct and try again. "
                    + "(This PowerShell subset implements: " + Cmdlets.SupportedList() + ".)");
                cnf.CommandName = name;
                WriteError(new ErrorRecord(cnf, "CommandNotFoundException", ErrorCategory.ObjectNotFound, name), null, cmd.Line);
                vars["?"] = false;
                return new List<object>();
            }

            StringBuilder args = new StringBuilder();
            foreach (CmdArg a in cmd.Args)
            {
                if (a.ParamName != null)
                {
                    Append(args, "-" + a.ParamName + (a.Value != null ? ":" + Values.ToPSString(Eval(a.Value)) : ""));
                    continue;
                }
                foreach (object o in Values.ToList(Eval(a.Value))) Append(args, Values.ToPSString(o));
            }
            if (input != null) foreach (object o in input) { /* stdin is not wired */ }

            string ext = Path.GetExtension(exe).ToLowerInvariant();
            System.Diagnostics.ProcessStartInfo psi;
            if (ext == ".bat" || ext == ".cmd")
                psi = new System.Diagnostics.ProcessStartInfo("cmd.exe", "/c \"" + exe + "\" " + args);
            else
                psi = new System.Diagnostics.ProcessStartInfo(exe, args.ToString());
            psi.UseShellExecute = false;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.CreateNoWindow = true;
            psi.WorkingDirectory = Directory.GetCurrentDirectory();

            Trace.Log("native: \"" + exe + "\" " + args);
            List<object> result = new List<object>();
            List<string> errLines = new List<string>();
            using (System.Diagnostics.Process proc = new System.Diagnostics.Process())
            {
                proc.StartInfo = psi;
                proc.OutputDataReceived += (sender, e) => { if (e.Data != null) lock (result) result.Add(e.Data); };
                proc.ErrorDataReceived += (sender, e) => { if (e.Data != null) lock (errLines) errLines.Add(e.Data); };
                try
                {
                    proc.Start();
                }
                catch (Exception ex)
                {
                    WriteError(new ErrorRecord(ex, "NativeCommandFailed", ErrorCategory.ResourceUnavailable, name), null, cmd.Line);
                    vars["?"] = false;
                    return result;
                }
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
                proc.WaitForExit();
                vars["LASTEXITCODE"] = proc.ExitCode;
                vars["?"] = proc.ExitCode == 0;
                Trace.Log("native: exit " + proc.ExitCode + ", " + result.Count + " stdout lines, " + errLines.Count + " stderr lines");
            }
            /* Windows PowerShell turns redirected stderr of a native command
             * into NativeCommandError records; hosts see them as errors. */
            foreach (string line in errLines)
                WriteError(new ErrorRecord(new RuntimeException(line), "NativeCommandError", ErrorCategory.NotSpecified, line), null, cmd.Line);
            return result;
        }

        private static void Append(StringBuilder sb, string arg)
        {
            if (sb.Length > 0) sb.Append(' ');
            if (arg.Length == 0 || arg.IndexOf(' ') >= 0 || arg.IndexOf('\t') >= 0)
                sb.Append('"').Append(arg.Replace("\"", "\\\"")).Append('"');
            else
                sb.Append(arg);
        }

        internal static string ResolveExecutable(string name)
        {
            string[] exts = { "", ".exe", ".com", ".bat", ".cmd" };
            if (name.IndexOf('\\') >= 0 || name.IndexOf('/') >= 0 || Path.IsPathRooted(name))
            {
                string full = Path.IsPathRooted(name) ? name : Path.Combine(Directory.GetCurrentDirectory(), name);
                foreach (string e in exts) if (File.Exists(full + e)) return Path.GetFullPath(full + e);
                return null;
            }
            List<string> dirs = new List<string>();
            dirs.Add(Directory.GetCurrentDirectory());
            string path = Environment.GetEnvironmentVariable("PATH") ?? "";
            foreach (string d in path.Split(';')) if (d.Trim().Length > 0) dirs.Add(d.Trim());
            foreach (string d in dirs)
            {
                foreach (string e in exts)
                {
                    if (e == "" && !Path.HasExtension(name)) continue;
                    string cand = Path.Combine(d, name + e);
                    if (File.Exists(cand)) return cand;
                }
            }
            return null;
        }

        /* ---------------- errors and streams ---------------- */

        public void WriteError(ErrorRecord rec, Invocation inv, int line)
        {
            string action = inv != null && inv.ErrorAction != null ? inv.ErrorAction : Values.ToPSString(vars["ErrorActionPreference"]);
            Trace.Log("error (line " + line + ", action " + action + "): " + rec);
            ((ArrayList)vars["Error"]).Insert(0, rec);
            vars["?"] = false;
            if (action.Equals("Ignore", StringComparison.OrdinalIgnoreCase)) return;
            if (action.Equals("SilentlyContinue", StringComparison.OrdinalIgnoreCase)) return;
            streams.Error.Add(rec);
            if (action.Equals("Stop", StringComparison.OrdinalIgnoreCase))
            {
                ActionPreferenceStopException stop = new ActionPreferenceStopException(rec.ToString());
                stop.ErrorRecord = rec;
                throw stop;
            }
        }

        public void WriteWarning(string message, Invocation inv)
        {
            Trace.Log("warning: " + message);
            string action = inv != null && inv.Has("WarningAction") ? inv.GetString("WarningAction") : Values.ToPSString(vars["WarningPreference"]);
            if (action.Equals("Ignore", StringComparison.OrdinalIgnoreCase) || action.Equals("SilentlyContinue", StringComparison.OrdinalIgnoreCase)) return;
            streams.Warning.Add(new WarningRecord(message));
        }

        public void WriteHost(string message, bool noNewline)
        {
            Trace.Log("host: " + message);
            HostInformationMessage him = new HostInformationMessage { Message = message, NoNewLine = noNewline };
            InformationRecord rec = new InformationRecord(him, "Write-Host");
            rec.Tags.Add("PSHOST");
            streams.Information.Add(rec);
        }

        public void WriteInformation(object data, string source)
        {
            streams.Information.Add(new InformationRecord(data, source));
        }

        public void WriteVerbose(string message) { streams.Verbose.Add(new VerboseRecord(message)); }
        public void WriteDebug(string message) { streams.Debug.Add(new DebugRecord(message)); }

        /* ---------------- variables ---------------- */

        public object GetVariable(VarExpr v)
        {
            if (v.Scope != null)
            {
                if (v.Scope.Equals("env", StringComparison.OrdinalIgnoreCase))
                    return Environment.GetEnvironmentVariable(v.Name);
                if (v.Scope.Equals("script", StringComparison.OrdinalIgnoreCase) || v.Scope.Equals("global", StringComparison.OrdinalIgnoreCase)
                    || v.Scope.Equals("local", StringComparison.OrdinalIgnoreCase) || v.Scope.Equals("private", StringComparison.OrdinalIgnoreCase))
                { object sv; return vars.TryGetValue(v.Name, out sv) ? sv : null; }
                throw new RuntimeException("Variable scope or drive '" + v.Scope + ":' is not supported by this PowerShell subset.");
            }
            object value;
            return vars.TryGetValue(v.Name, out value) ? value : null;
        }

        public void SetVariable(VarExpr v, object value)
        {
            if (v.Scope != null)
            {
                if (v.Scope.Equals("env", StringComparison.OrdinalIgnoreCase))
                {
                    Environment.SetEnvironmentVariable(v.Name, value == null ? null : Values.ToPSString(value));
                    return;
                }
                if (v.Scope.Equals("script", StringComparison.OrdinalIgnoreCase) || v.Scope.Equals("global", StringComparison.OrdinalIgnoreCase)
                    || v.Scope.Equals("local", StringComparison.OrdinalIgnoreCase) || v.Scope.Equals("private", StringComparison.OrdinalIgnoreCase))
                { vars[v.Name] = value; return; }
                throw new RuntimeException("Variable scope or drive '" + v.Scope + ":' is not supported by this PowerShell subset.");
            }
            if (v.Name == "?" || v.Name == "true" || v.Name == "false" || v.Name == "null")
                throw new RuntimeException("Cannot overwrite variable " + v.Name + " because it is read-only or constant.");
            vars[v.Name] = value;
        }

        public object GetVariable(string name) { object v; return vars.TryGetValue(name, out v) ? v : null; }
        public void SetVariable(string name, object value) { vars[name] = value; }

        /* ---------------- expressions ---------------- */

        public object Eval(Expr e)
        {
            Literal lit = e as Literal;
            if (lit != null) return lit.Value;

            VarExpr v = e as VarExpr;
            if (v != null) return GetVariable(v);

            InterpolatedString str = e as InterpolatedString;
            if (str != null)
            {
                StringBuilder sb = new StringBuilder();
                foreach (Expr part in str.Parts) sb.Append(Values.ToPSString(Eval(part)));
                return sb.ToString();
            }

            ArrayExpr arr = e as ArrayExpr;
            if (arr != null)
            {
                object[] items = new object[arr.Items.Count];
                for (int i = 0; i < items.Length; i++) items[i] = Eval(arr.Items[i]);
                return items;
            }

            ParenExpr paren = e as ParenExpr;
            if (paren != null)
            {
                List<object> list = paren.Inner == null ? new List<object>() : RunPipeline(paren.Inner);
                if (paren.ForceArray) return list.ToArray();
                return Values.FromList(list);
            }

            Pipeline pipe = e as Pipeline;
            if (pipe != null) return Values.FromList(RunPipeline(pipe));

            CommandExpr cmd = e as CommandExpr;
            if (cmd != null) return Values.FromList(RunCommand(cmd, null));

            UnaryExpr un = e as UnaryExpr;
            if (un != null)
            {
                object o = Eval(un.Operand);
                if (un.Op == "not") return !Values.ToBool(o);
                if (un.Op == "-")
                {
                    double d;
                    if (!Values.TryToNumber(o, out d)) throw new RuntimeException("Cannot negate value \"" + Values.ToPSString(o) + "\" (line " + un.Line + ").");
                    return Values.Normalize(-d);
                }
                throw new RuntimeException("Unknown unary operator '" + un.Op + "'.");
            }

            BinaryExpr bin = e as BinaryExpr;
            if (bin != null) return EvalBinary(bin);

            MemberExpr mem = e as MemberExpr;
            if (mem != null) return EvalMember(mem);

            IndexExpr idx = e as IndexExpr;
            if (idx != null)
            {
                object target = Values.Unwrap(Eval(idx.Target));
                object index = Values.Unwrap(Eval(idx.Index));
                if (target == null) return null;
                IDictionary dict = target as IDictionary;
                if (dict != null)
                {
                    foreach (DictionaryEntry de in dict)
                        if (Values.AreEqual(de.Key, index, false)) return de.Value;
                    return null;
                }
                int i = Values.ToInt(index, "index");
                string s = target as string;
                if (s != null) { if (i < 0) i += s.Length; return i >= 0 && i < s.Length ? (object)s[i].ToString() : null; }
                IList list = target as IList;
                if (list != null) { if (i < 0) i += list.Count; return i >= 0 && i < list.Count ? list[i] : null; }
                if (i == 0) return target;
                return null;
            }

            ScriptBlockExpr sbe = e as ScriptBlockExpr;
            if (sbe != null) return sbe;

            throw new RuntimeException("Internal error: unknown expression type " + e.GetType().Name);
        }

        private object EvalBinary(BinaryExpr bin)
        {
            string op = bin.Op;
            if (op == "and") return Values.ToBool(Eval(bin.Left)) && Values.ToBool(Eval(bin.Right));
            if (op == "or") return Values.ToBool(Eval(bin.Left)) || Values.ToBool(Eval(bin.Right));
            if (op == "xor") return Values.ToBool(Eval(bin.Left)) ^ Values.ToBool(Eval(bin.Right));

            object l = Eval(bin.Left);
            object r = Eval(bin.Right);
            bool cs = false;
            if (op.Length > 2 && op[0] == 'c' && comparisonNames.Contains(op.Substring(1))) { cs = true; op = op.Substring(1); }

            switch (op)
            {
                case "+": case "-": case "*": case "/": case "%":
                    return Arithmetic(op, l, r);
                case "eq": return CompareCollection(l, r, cs, true);
                case "ne": return CompareCollection(l, r, cs, false);
                case "gt": return Values.Compare(l, r, cs) > 0;
                case "ge": return Values.Compare(l, r, cs) >= 0;
                case "lt": return Values.Compare(l, r, cs) < 0;
                case "le": return Values.Compare(l, r, cs) <= 0;
                case "like": return Values.WildcardToRegex(Values.ToPSString(r), cs).IsMatch(Values.ToPSString(l));
                case "notlike": return !Values.WildcardToRegex(Values.ToPSString(r), cs).IsMatch(Values.ToPSString(l));
                case "match":
                case "notmatch":
                {
                    System.Text.RegularExpressions.Regex rx = new System.Text.RegularExpressions.Regex(Values.ToPSString(r),
                        cs ? System.Text.RegularExpressions.RegexOptions.None : System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                    bool m = rx.IsMatch(Values.ToPSString(l));
                    return op == "match" ? m : !m;
                }
                case "contains":
                case "notcontains":
                {
                    bool found = false;
                    foreach (object o in Values.ToList(l)) if (Values.AreEqual(o, r, cs)) { found = true; break; }
                    return op == "contains" ? found : !found;
                }
                case "in":
                case "notin":
                {
                    bool found = false;
                    foreach (object o in Values.ToList(r)) if (Values.AreEqual(o, l, cs)) { found = true; break; }
                    return op == "in" ? found : !found;
                }
                case "replace":
                {
                    List<object> args = Values.ToList(r);
                    string pattern = args.Count > 0 ? Values.ToPSString(args[0]) : "";
                    string repl = args.Count > 1 ? Values.ToPSString(args[1]) : "";
                    return System.Text.RegularExpressions.Regex.Replace(Values.ToPSString(l), pattern, repl,
                        cs ? System.Text.RegularExpressions.RegexOptions.None : System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                }
                case "split":
                    return System.Text.RegularExpressions.Regex.Split(Values.ToPSString(l), Values.ToPSString(r));
                case "join":
                {
                    StringBuilder sb = new StringBuilder();
                    string sep = Values.ToPSString(r);
                    foreach (object o in Values.ToList(l)) { if (sb.Length > 0) sb.Append(sep); sb.Append(Values.ToPSString(o)); }
                    return sb.ToString();
                }
            }
            throw new RuntimeException("Operator '-" + bin.Op + "' is not supported by this PowerShell subset (line " + bin.Line + ").");
        }

        private static readonly HashSet<string> comparisonNames = new HashSet<string>
        {
            "eq", "ne", "gt", "ge", "lt", "le", "like", "notlike", "match", "notmatch", "contains", "notcontains", "in", "notin", "replace", "split"
        };

        /* -eq against a collection filters it; against a scalar it is a boolean. */
        private static object CompareCollection(object l, object r, bool cs, bool wantEqual)
        {
            object ul = Values.Unwrap(l);
            if (ul != null && !(ul is string) && ul is IEnumerable && !(ul is IDictionary))
            {
                List<object> matches = new List<object>();
                foreach (object o in (IEnumerable)ul) if (Values.AreEqual(o, r, cs) == wantEqual) matches.Add(o);
                return matches.ToArray();
            }
            return Values.AreEqual(l, r, cs) == wantEqual;
        }

        internal static object Arithmetic(string op, object l, object r)
        {
            l = Values.Unwrap(l); r = Values.Unwrap(r);
            if (op == "+")
            {
                if (l is string || (l == null && r is string)) return Values.ToPSString(l) + Values.ToPSString(r);
                if (l != null && !(l is string) && l is IEnumerable && !(l is IDictionary))
                {
                    List<object> list = Values.ToList(l);
                    list.AddRange(Values.ToList(r));
                    return list.ToArray();
                }
            }
            if (op == "*" && l is string) { int n = Values.ToInt(r, "repeat count"); StringBuilder sb = new StringBuilder(); for (int i = 0; i < n; i++) sb.Append((string)l); return sb.ToString(); }
            double dl, dr;
            if (!Values.TryToNumber(l, out dl))
            {
                if (op == "+" ) return Values.ToPSString(l) + Values.ToPSString(r);
                throw new RuntimeException("Cannot convert value \"" + Values.ToPSString(l) + "\" to type \"System.Double\" for operator '" + op + "'.");
            }
            if (!Values.TryToNumber(r, out dr))
            {
                if (op == "+") return Values.ToPSString(l) + Values.ToPSString(r);
                throw new RuntimeException("Cannot convert value \"" + Values.ToPSString(r) + "\" to type \"System.Double\" for operator '" + op + "'.");
            }
            switch (op)
            {
                case "+": return Values.Normalize(dl + dr);
                case "-": return Values.Normalize(dl - dr);
                case "*": return Values.Normalize(dl * dr);
                case "/": if (dr == 0) throw new RuntimeException("Attempted to divide by zero."); return Values.Normalize(dl / dr);
                case "%": if (dr == 0) throw new RuntimeException("Attempted to divide by zero."); return Values.Normalize(dl % dr);
            }
            throw new RuntimeException("Unknown operator '" + op + "'.");
        }

        /* ---------------- members ---------------- */

        private object EvalMember(MemberExpr mem)
        {
            object target = Values.Unwrap(Eval(mem.Target));
            string name = mem.Name;
            if (target == null)
            {
                if (mem.IsCall) throw new RuntimeException("You cannot call a method on a null-valued expression (." + name + ", line " + mem.Line + ").");
                return null;
            }
            if (!mem.IsCall)
            {
                /* synthetic members PowerShell adds to everything */
                if (name.Equals("Count", StringComparison.OrdinalIgnoreCase) || name.Equals("Length", StringComparison.OrdinalIgnoreCase))
                {
                    string s = target as string;
                    if (s != null && name.Equals("Length", StringComparison.OrdinalIgnoreCase)) return s.Length;
                    ICollection col = target as ICollection;
                    if (col != null) return col.Count;
                    if (s != null) return 1;
                    if (!HasMember(target, name)) return 1;
                }
                IDictionary dict = target as IDictionary;
                if (dict != null)
                {
                    foreach (DictionaryEntry de in dict) if (Values.AreEqual(de.Key, name, false)) return de.Value;
                    if (name.Equals("Keys", StringComparison.OrdinalIgnoreCase)) return dict.Keys;
                    if (name.Equals("Values", StringComparison.OrdinalIgnoreCase)) return dict.Values;
                    return null;
                }
                Type t = target.GetType();
                PropertyInfo pi = t.GetProperty(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.IgnoreCase);
                if (pi != null && pi.GetIndexParameters().Length == 0)
                {
                    try { return pi.GetValue(target, null); }
                    catch (TargetInvocationException tie) { throw new RuntimeException("Exception getting \"" + name + "\": \"" + tie.InnerException.Message + "\"", tie.InnerException); }
                }
                FieldInfo fi = t.GetField(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.IgnoreCase);
                if (fi != null) return fi.GetValue(target);
                /* Unknown property of an array applies to each element (member enumeration). */
                if (!(target is string) && target is IEnumerable)
                {
                    List<object> res = new List<object>();
                    foreach (object o in (IEnumerable)target)
                    {
                        if (o == null) continue;
                        PropertyInfo epi = o.GetType().GetProperty(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.IgnoreCase);
                        if (epi != null) res.Add(epi.GetValue(o, null));
                    }
                    return res.Count == 0 ? null : Values.FromList(res);
                }
                return null;     /* PowerShell yields $null for unknown properties */
            }

            object[] args = new object[mem.Args.Count];
            for (int i = 0; i < args.Length; i++) args[i] = Values.Unwrap(Eval(mem.Args[i]));
            try
            {
                Type t = target.GetType();
                MethodInfo best = PickMethod(t, name, args);
                if (best == null)
                    throw new RuntimeException("Method invocation failed because [" + t.FullName + "] does not contain a method named '" + name + "' taking " + args.Length + " argument(s) (line " + mem.Line + ").");
                ParameterInfo[] ps = best.GetParameters();
                object[] conv = new object[ps.Length];
                for (int i = 0; i < ps.Length; i++) conv[i] = ConvertArg(args[i], ps[i].ParameterType);
                object result = best.Invoke(target, conv);
                return best.ReturnType == typeof(void) ? null : result;
            }
            catch (TargetInvocationException tie)
            {
                throw new RuntimeException("Exception calling \"" + name + "\": \"" + tie.InnerException.Message + "\"", tie.InnerException);
            }
        }

        private static bool HasMember(object target, string name)
        {
            Type t = target.GetType();
            return t.GetProperty(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.IgnoreCase) != null
                || t.GetField(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.IgnoreCase) != null;
        }

        private static MethodInfo PickMethod(Type t, string name, object[] args)
        {
            MethodInfo fallback = null;
            foreach (MethodInfo mi in t.GetMethods(BindingFlags.Public | BindingFlags.Instance))
            {
                if (!mi.Name.Equals(name, StringComparison.OrdinalIgnoreCase)) continue;
                ParameterInfo[] ps = mi.GetParameters();
                if (ps.Length != args.Length) continue;
                bool exact = true;
                for (int i = 0; i < ps.Length; i++)
                {
                    if (args[i] == null) { if (ps[i].ParameterType.IsValueType) { exact = false; break; } continue; }
                    if (!ps[i].ParameterType.IsAssignableFrom(args[i].GetType())) { exact = false; break; }
                }
                if (exact) return mi;
                if (fallback == null) fallback = mi;
            }
            return fallback;
        }

        internal static object ConvertArg(object v, Type target)
        {
            if (v == null) return null;
            if (target.IsAssignableFrom(v.GetType())) return v;
            if (target == typeof(string)) return Values.ToPSString(v);
            if (target == typeof(bool)) return Values.ToBool(v);
            if (target.IsEnum) return Enum.Parse(target, Values.ToPSString(v), true);
            if (target.IsPrimitive || target == typeof(decimal))
            {
                double d;
                if (Values.TryToNumber(v, out d)) return Convert.ChangeType(d, target, CultureInfo.InvariantCulture);
            }
            if (target == typeof(char) && v is string && ((string)v).Length == 1) return ((string)v)[0];
            if (target == typeof(char[]) && v is string) return ((string)v).ToCharArray();
            if (target == typeof(string[]))
            {
                List<object> list = Values.ToList(v);
                string[] arr = new string[list.Count];
                for (int i = 0; i < arr.Length; i++) arr[i] = Values.ToPSString(list[i]);
                return arr;
            }
            return Convert.ChangeType(v, target, CultureInfo.InvariantCulture);
        }

        /* ---------------- tracing helpers ---------------- */

        internal static string Describe(object v)
        {
            v = Values.Unwrap(v);
            if (v == null) return "$null";
            if (v is string) return "\"" + v + "\"";
            if (v is bool || Values.IsNumber(v)) return Values.ToPSString(v);
            if (v is System.Diagnostics.Process)
            {
                System.Diagnostics.Process pr = (System.Diagnostics.Process)v;
                string state;
                try { state = pr.HasExited ? "exited " + pr.ExitCode : "running"; } catch (Exception) { state = "?"; }
                return "[Process " + state + "]";
            }
            if (v is IEnumerable && !(v is IDictionary)) return "@(" + Values.ToPSString(v) + ")";
            return "[" + v.GetType().Name + "] " + Values.ToPSString(v);
        }

        private static string DescribeBound(Invocation inv)
        {
            StringBuilder sb = new StringBuilder();
            foreach (KeyValuePair<string, object> kv in inv.Bound)
            {
                if (sb.Length > 0) sb.Append(' ');
                sb.Append('-').Append(kv.Key).Append(' ').Append(Describe(kv.Value));
            }
            return sb.ToString();
        }
    }
}
