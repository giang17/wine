/*
 * Cmdlets of the PowerShell subset (see Api.cs).
 *
 * The set is what installer prerun scripts use: host output, path tests,
 * process start/stop/wait, sleep, a few path and item helpers. Anything else
 * is looked up as a native executable and, failing that, reported as
 * CommandNotFoundException -- the honest outcome, so that a hosting installer
 * reports the script as failed instead of assuming it ran.
 */

using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;

namespace System.Management.Automation.Shim
{
    internal sealed class CmdletParam
    {
        public string Name;
        public string[] Aliases = new string[0];
        public bool IsSwitch;
        public bool Mandatory;
        public int Position = -1;
        public string TypeName = "System.Object";

        public bool Matches(string given)
        {
            if (Name.Equals(given, StringComparison.OrdinalIgnoreCase)) return true;
            foreach (string a in Aliases) if (a.Equals(given, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }
    }

    internal delegate List<object> CmdletBody(Invocation inv);

    internal sealed class Cmdlet
    {
        public string Name;
        public string[] Aliases = new string[0];
        public List<CmdletParam> Params = new List<CmdletParam>();
        public string CollectsRemaining;       /* parameter that takes all surplus positional values */
        public CmdletBody Run;

        public Cmdlet(string name, params string[] aliases) { Name = name; Aliases = aliases; }

        public Cmdlet Value(string name, int position, params string[] aliases)
        {
            Params.Add(new CmdletParam { Name = name, Position = position, Aliases = aliases, TypeName = "System.Object" });
            return this;
        }
        public Cmdlet Switch(string name, params string[] aliases)
        {
            Params.Add(new CmdletParam { Name = name, IsSwitch = true, Aliases = aliases, TypeName = "System.Management.Automation.SwitchParameter" });
            return this;
        }
        public Cmdlet Required(string name)
        {
            foreach (CmdletParam p in Params) if (p.Name == name) p.Mandatory = true;
            return this;
        }
        public Cmdlet Rest(string name) { CollectsRemaining = name; return this; }
        public Cmdlet Body(CmdletBody body) { Run = body; return this; }

        /* Unique prefix match, as PowerShell binds parameters. */
        public CmdletParam Resolve(string given)
        {
            foreach (CmdletParam p in Params) if (p.Matches(given)) return p;
            CmdletParam found = null;
            foreach (CmdletParam p in Params)
            {
                if (p.Name.StartsWith(given, StringComparison.OrdinalIgnoreCase))
                {
                    if (found != null) return null;
                    found = p;
                }
            }
            return found;
        }

        public CmdletParam Positional(int index)
        {
            foreach (CmdletParam p in Params) if (p.Position == index) return p;
            return null;
        }
    }

    internal static class Cmdlets
    {
        private static readonly Dictionary<string, Cmdlet> table = new Dictionary<string, Cmdlet>(StringComparer.OrdinalIgnoreCase);
        private static readonly List<Cmdlet> ordered = new List<Cmdlet>();

        private static void Add(Cmdlet c)
        {
            ordered.Add(c);
            table[c.Name] = c;
            foreach (string a in c.Aliases) table[a] = c;
        }

        public static Cmdlet Find(string name)
        {
            Cmdlet c;
            return table.TryGetValue(name, out c) ? c : null;
        }

        public static string SupportedList()
        {
            StringBuilder sb = new StringBuilder();
            foreach (Cmdlet c in ordered) { if (sb.Length > 0) sb.Append(", "); sb.Append(c.Name); }
            return sb.ToString();
        }

        private static List<object> None() { return new List<object>(); }
        private static List<object> One(object o) { List<object> l = new List<object>(); l.Add(o); return l; }

        private static string Join(Invocation inv, string param, string separator)
        {
            StringBuilder sb = new StringBuilder();
            foreach (object o in inv.GetList(param)) { if (sb.Length > 0) sb.Append(separator); sb.Append(Values.ToPSString(o)); }
            return sb.ToString();
        }

        static Cmdlets()
        {
            /* ---- output ---- */

            Add(new Cmdlet("Write-Host")
                .Value("Object", 0).Rest("Object").Value("Separator", -1).Value("ForegroundColor", -1).Value("BackgroundColor", -1).Switch("NoNewline")
                .Body(inv =>
                {
                    string sep = inv.Has("Separator") ? inv.GetString("Separator") : " ";
                    inv.Interp.WriteHost(Join(inv, "Object", sep), inv.GetSwitch("NoNewline"));
                    return None();
                }));

            Add(new Cmdlet("Write-Output", "echo", "write")
                .Value("InputObject", 0).Rest("InputObject").Switch("NoEnumerate")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    if (inv.Has("InputObject")) res.AddRange(Values.ToList(inv.Get("InputObject")));
                    if (inv.Input != null) res.AddRange(inv.Input);
                    return res;
                }));

            Add(new Cmdlet("Write-Error")
                .Value("Message", 0, "Msg").Value("Exception", -1).Value("ErrorId", -1).Value("Category", -1).Value("TargetObject", -1)
                .Value("RecommendedAction", -1).Value("CategoryActivity", -1).Value("CategoryReason", -1).Value("ErrorRecord", -1)
                .Body(inv =>
                {
                    ErrorRecord rec = inv.Get("ErrorRecord") as ErrorRecord;
                    if (rec == null)
                    {
                        string msg = inv.Has("Message") ? inv.GetString("Message") : "";
                        Exception ex = inv.Get("Exception") as Exception;
                        if (ex == null) ex = new WriteErrorException(msg.Length > 0 ? msg : "Write-Error");
                        ErrorCategory cat = ErrorCategory.NotSpecified;
                        if (inv.Has("Category")) { try { cat = (ErrorCategory)Enum.Parse(typeof(ErrorCategory), inv.GetString("Category"), true); } catch (Exception) { } }
                        string id = inv.Has("ErrorId") ? inv.GetString("ErrorId") : "Microsoft.PowerShell.Commands.WriteErrorException";
                        rec = new ErrorRecord(ex, id, cat, inv.Get("TargetObject"));
                        if (msg.Length > 0 && inv.Has("Exception")) rec.ErrorDetails = new ErrorDetails(msg);
                    }
                    inv.Interp.WriteError(rec, inv, inv.Line);
                    return None();
                }));

            Add(new Cmdlet("Write-Warning")
                .Value("Message", 0, "Msg").Required("Message")
                .Body(inv => { inv.Interp.WriteWarning(inv.GetString("Message"), inv); return None(); }));

            Add(new Cmdlet("Write-Verbose")
                .Value("Message", 0, "Msg").Required("Message")
                .Body(inv => { inv.Interp.WriteVerbose(inv.GetString("Message")); return None(); }));

            Add(new Cmdlet("Write-Debug")
                .Value("Message", 0, "Msg").Required("Message")
                .Body(inv => { inv.Interp.WriteDebug(inv.GetString("Message")); return None(); }));

            Add(new Cmdlet("Write-Information")
                .Value("MessageData", 0, "Msg").Value("Tags", 1)
                .Body(inv => { inv.Interp.WriteInformation(inv.Get("MessageData"), "Write-Information"); return None(); }));

            Add(new Cmdlet("Write-Progress")
                .Value("Activity", 0).Value("Status", 1).Value("Id", 2).Value("PercentComplete", -1).Value("CurrentOperation", -1).Switch("Completed")
                .Body(inv => None()));

            Add(new Cmdlet("Out-Null")
                .Value("InputObject", -1)
                .Body(inv => None()));

            Add(new Cmdlet("Out-Host", "Out-Default")
                .Value("InputObject", -1).Switch("Paging")
                .Body(inv =>
                {
                    List<object> items = inv.Input ?? Values.ToList(inv.Get("InputObject"));
                    foreach (object o in items) inv.Interp.WriteHost(Values.ToPSString(o), false);
                    return None();
                }));

            Add(new Cmdlet("Out-String")
                .Value("InputObject", -1).Switch("Stream").Value("Width", -1)
                .Body(inv =>
                {
                    List<object> items = inv.Input ?? Values.ToList(inv.Get("InputObject"));
                    if (inv.GetSwitch("Stream")) { List<object> res = new List<object>(); foreach (object o in items) res.Add(Values.ToPSString(o)); return res; }
                    StringBuilder sb = new StringBuilder();
                    foreach (object o in items) sb.Append(Values.ToPSString(o)).Append("\r\n");
                    return One(sb.ToString());
                }));

            /* ---- paths and items ---- */

            Add(new Cmdlet("Test-Path")
                .Value("Path", 0, "LiteralPath", "PSPath").Required("Path").Value("PathType", -1, "Type").Value("Filter", -1).Value("Include", -1).Value("Exclude", -1).Switch("IsValid")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    string type = inv.Has("PathType") ? inv.GetString("PathType") : "Any";
                    foreach (object o in inv.GetList("Path"))
                    {
                        string path = Values.ToPSString(o);
                        bool result;
                        if (inv.GetSwitch("IsValid")) result = path.Length > 0 && path.IndexOfAny(Path.GetInvalidPathChars()) < 0;
                        else if (type.Equals("Leaf", StringComparison.OrdinalIgnoreCase)) result = File.Exists(path);
                        else if (type.Equals("Container", StringComparison.OrdinalIgnoreCase)) result = Directory.Exists(path);
                        else result = File.Exists(path) || Directory.Exists(path);
                        Trace.Log("Test-Path \"" + path + "\" -> " + result);
                        res.Add(result);
                    }
                    return res;
                }));

            Add(new Cmdlet("Join-Path")
                .Value("Path", 0).Required("Path").Value("ChildPath", 1).Required("ChildPath").Switch("Resolve")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    foreach (object o in inv.GetList("Path")) res.Add(Path.Combine(Values.ToPSString(o), inv.GetString("ChildPath")));
                    return res;
                }));

            Add(new Cmdlet("Split-Path")
                .Value("Path", 0, "LiteralPath").Required("Path").Switch("Parent").Switch("Leaf").Switch("Extension").Switch("Qualifier").Switch("NoQualifier").Switch("LeafBase").Switch("IsAbsolute").Switch("Resolve")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    foreach (object o in inv.GetList("Path"))
                    {
                        string path = Values.ToPSString(o);
                        if (inv.GetSwitch("Leaf")) res.Add(Path.GetFileName(path));
                        else if (inv.GetSwitch("LeafBase")) res.Add(Path.GetFileNameWithoutExtension(path));
                        else if (inv.GetSwitch("Extension")) res.Add(Path.GetExtension(path));
                        else if (inv.GetSwitch("Qualifier")) res.Add(Path.GetPathRoot(path).TrimEnd('\\'));
                        else if (inv.GetSwitch("NoQualifier")) res.Add(path.Substring(Path.GetPathRoot(path).Length - 1));
                        else if (inv.GetSwitch("IsAbsolute")) res.Add(Path.IsPathRooted(path));
                        else res.Add(Path.GetDirectoryName(path) ?? "");
                    }
                    return res;
                }));

            Add(new Cmdlet("Get-Location", "pwd")
                .Body(inv => One(Directory.GetCurrentDirectory())));

            Add(new Cmdlet("Set-Location", "cd", "chdir", "sl")
                .Value("Path", 0, "LiteralPath").Switch("PassThru")
                .Body(inv =>
                {
                    string path = inv.Has("Path") ? inv.GetString("Path") : Values.ToPSString(inv.Interp.GetVariable("HOME"));
                    if (!Directory.Exists(path))
                    {
                        inv.Interp.WriteError(new ErrorRecord(new DirectoryNotFoundException("Cannot find path '" + path + "' because it does not exist."), "PathNotFound", ErrorCategory.ObjectNotFound, path), inv, inv.Line);
                        return None();
                    }
                    Directory.SetCurrentDirectory(path);
                    inv.Interp.SetVariable("PWD", path);
                    return inv.GetSwitch("PassThru") ? One(path) : None();
                }));

            Add(new Cmdlet("New-Item", "ni")
                .Value("Path", 0).Required("Path").Value("Name", -1).Value("ItemType", -1, "Type").Value("Value", -1).Switch("Force")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    string type = inv.Has("ItemType") ? inv.GetString("ItemType") : "File";
                    foreach (object o in inv.GetList("Path"))
                    {
                        string path = Values.ToPSString(o);
                        if (inv.Has("Name")) path = Path.Combine(path, inv.GetString("Name"));
                        try
                        {
                            if (type.Equals("Directory", StringComparison.OrdinalIgnoreCase))
                            {
                                if (Directory.Exists(path) && !inv.GetSwitch("Force"))
                                {
                                    inv.Interp.WriteError(new ErrorRecord(new IOException("An item with the specified name " + path + " already exists."), "DirectoryExist", ErrorCategory.ResourceExists, path), inv, inv.Line);
                                    continue;
                                }
                                Directory.CreateDirectory(path);
                                res.Add(new DirectoryInfo(path));
                            }
                            else
                            {
                                if (File.Exists(path) && !inv.GetSwitch("Force"))
                                {
                                    inv.Interp.WriteError(new ErrorRecord(new IOException("The file '" + path + "' already exists."), "NewItemIOError", ErrorCategory.ResourceExists, path), inv, inv.Line);
                                    continue;
                                }
                                string dir = Path.GetDirectoryName(path);
                                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir) && inv.GetSwitch("Force")) Directory.CreateDirectory(dir);
                                File.WriteAllText(path, inv.Has("Value") ? inv.GetString("Value") : "");
                                res.Add(new FileInfo(path));
                            }
                        }
                        catch (Exception ex)
                        {
                            inv.Interp.WriteError(new ErrorRecord(ex, "NewItemIOError", ErrorCategory.WriteError, path), inv, inv.Line);
                        }
                    }
                    return res;
                }));

            Add(new Cmdlet("Remove-Item", "ri", "rm", "del", "erase", "rmdir", "rd")
                .Value("Path", 0, "LiteralPath").Required("Path").Switch("Recurse").Switch("Force").Value("Filter", -1).Value("Include", -1).Value("Exclude", -1)
                .Body(inv =>
                {
                    foreach (object o in inv.GetList("Path"))
                    {
                        string path = Values.ToPSString(o);
                        try
                        {
                            if (Directory.Exists(path)) Directory.Delete(path, inv.GetSwitch("Recurse"));
                            else if (File.Exists(path)) { if (inv.GetSwitch("Force")) File.SetAttributes(path, FileAttributes.Normal); File.Delete(path); }
                            else inv.Interp.WriteError(new ErrorRecord(new FileNotFoundException("Cannot find path '" + path + "' because it does not exist."), "PathNotFound", ErrorCategory.ObjectNotFound, path), inv, inv.Line);
                        }
                        catch (Exception ex)
                        {
                            inv.Interp.WriteError(new ErrorRecord(ex, "RemoveItemIOError", ErrorCategory.WriteError, path), inv, inv.Line);
                        }
                    }
                    return None();
                }));

            Add(new Cmdlet("Copy-Item", "cp", "copy", "cpi")
                .Value("Path", 0, "LiteralPath").Required("Path").Value("Destination", 1).Required("Destination").Switch("Force").Switch("Recurse").Switch("PassThru")
                .Body(inv =>
                {
                    string dest = inv.GetString("Destination");
                    foreach (object o in inv.GetList("Path"))
                    {
                        string src = Values.ToPSString(o);
                        try
                        {
                            if (Directory.Exists(src)) CopyDirectory(src, Directory.Exists(dest) ? Path.Combine(dest, Path.GetFileName(src)) : dest, inv.GetSwitch("Force"));
                            else if (File.Exists(src)) File.Copy(src, Directory.Exists(dest) ? Path.Combine(dest, Path.GetFileName(src)) : dest, inv.GetSwitch("Force"));
                            else inv.Interp.WriteError(new ErrorRecord(new FileNotFoundException("Cannot find path '" + src + "' because it does not exist."), "PathNotFound", ErrorCategory.ObjectNotFound, src), inv, inv.Line);
                        }
                        catch (Exception ex)
                        {
                            inv.Interp.WriteError(new ErrorRecord(ex, "CopyItemIOError", ErrorCategory.WriteError, src), inv, inv.Line);
                        }
                    }
                    return None();
                }));

            Add(new Cmdlet("Get-Content", "gc", "cat", "type")
                .Value("Path", 0, "LiteralPath").Required("Path").Switch("Raw").Value("Encoding", -1).Value("TotalCount", -1, "First", "Head").Value("Tail", -1, "Last")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    foreach (object o in inv.GetList("Path"))
                    {
                        string path = Values.ToPSString(o);
                        if (!File.Exists(path))
                        {
                            inv.Interp.WriteError(new ErrorRecord(new FileNotFoundException("Cannot find path '" + path + "' because it does not exist."), "PathNotFound", ErrorCategory.ObjectNotFound, path), inv, inv.Line);
                            continue;
                        }
                        if (inv.GetSwitch("Raw")) { res.Add(File.ReadAllText(path)); continue; }
                        string[] lines = File.ReadAllLines(path);
                        int start = 0, count = lines.Length;
                        if (inv.Has("TotalCount")) count = Math.Min(count, Values.ToInt(inv.Get("TotalCount"), "TotalCount"));
                        if (inv.Has("Tail")) { int t = Values.ToInt(inv.Get("Tail"), "Tail"); start = Math.Max(0, lines.Length - t); count = lines.Length - start; }
                        for (int i = start; i < start + count; i++) res.Add(lines[i]);
                    }
                    return res;
                }));

            Add(new Cmdlet("Get-Date")
                .Value("Date", 0).Value("Format", -1).Value("UFormat", -1)
                .Body(inv =>
                {
                    DateTime d = DateTime.Now;
                    if (inv.Has("Format")) return One(d.ToString(inv.GetString("Format")));
                    return One(d);
                }));

            /* ---- processes ---- */

            Add(new Cmdlet("Start-Process", "start", "saps")
                .Value("FilePath", 0, "PSPath", "Path").Required("FilePath").Value("ArgumentList", 1, "Args")
                .Value("WorkingDirectory", -1).Value("WindowStyle", -1).Value("Verb", -1)
                .Value("RedirectStandardOutput", -1, "RSO").Value("RedirectStandardError", -1, "RSE").Value("RedirectStandardInput", -1, "RSI")
                .Switch("Wait").Switch("PassThru").Switch("NoNewWindow", "nnw").Switch("LoadUserProfile", "Lup").Switch("UseNewEnvironment")
                .Body(StartProcess));

            Add(new Cmdlet("Get-Process", "gps", "ps")
                .Value("Name", 0, "ProcessName").Value("Id", -1, "PID").Value("InputObject", -1)
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    if (inv.Has("Id"))
                    {
                        foreach (object o in inv.GetList("Id"))
                        {
                            int id = Values.ToInt(o, "Id");
                            try { res.Add(Process.GetProcessById(id)); }
                            catch (Exception) { inv.Interp.WriteError(new ErrorRecord(new RuntimeException("Cannot find a process with the process identifier " + id + "."), "NoProcessFoundForGivenId", ErrorCategory.ObjectNotFound, id), inv, inv.Line); }
                        }
                        return res;
                    }
                    Process[] all = Process.GetProcesses();
                    if (!inv.Has("Name")) { res.AddRange(all); return res; }
                    foreach (object o in inv.GetList("Name"))
                    {
                        string pattern = Values.ToPSString(o);
                        if (pattern.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) pattern = pattern.Substring(0, pattern.Length - 4);
                        System.Text.RegularExpressions.Regex rx = Values.WildcardToRegex(pattern, false);
                        int before = res.Count;
                        foreach (Process pr in all)
                        {
                            string pn;
                            try { pn = pr.ProcessName; } catch (Exception) { continue; }
                            if (pn.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) pn = pn.Substring(0, pn.Length - 4);
                            if (rx.IsMatch(pn)) res.Add(pr);
                        }
                        if (res.Count == before && pattern.IndexOfAny(new[] { '*', '?' }) < 0)
                            inv.Interp.WriteError(new ErrorRecord(new RuntimeException("Cannot find a process with the name \"" + pattern + "\". Verify the process name and call the cmdlet again."), "NoProcessFoundForGivenName", ErrorCategory.ObjectNotFound, pattern), inv, inv.Line);
                    }
                    Trace.Log("Get-Process -> " + res.Count + " process(es)");
                    return res;
                }));

            Add(new Cmdlet("Stop-Process", "kill", "spps")
                .Value("Id", 0).Value("Name", -1, "ProcessName").Value("InputObject", -1).Switch("Force").Switch("PassThru")
                .Body(inv =>
                {
                    List<Process> targets = new List<Process>();
                    List<object> res = new List<object>();
                    if (inv.Input != null) foreach (object o in inv.Input) { Process pr = Values.Unwrap(o) as Process; if (pr != null) targets.Add(pr); }
                    foreach (object o in inv.GetList("InputObject")) { Process pr = Values.Unwrap(o) as Process; if (pr != null) targets.Add(pr); }
                    foreach (object o in inv.GetList("Id"))
                    {
                        Process pr = Values.Unwrap(o) as Process;
                        if (pr != null) { targets.Add(pr); continue; }
                        int id = Values.ToInt(o, "Id");
                        try { targets.Add(Process.GetProcessById(id)); }
                        catch (Exception) { inv.Interp.WriteError(new ErrorRecord(new RuntimeException("Cannot find a process with the process identifier " + id + "."), "NoProcessFoundForGivenId", ErrorCategory.ObjectNotFound, id), inv, inv.Line); }
                    }
                    foreach (object o in inv.GetList("Name"))
                    {
                        string pattern = Values.ToPSString(o);
                        if (pattern.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) pattern = pattern.Substring(0, pattern.Length - 4);
                        System.Text.RegularExpressions.Regex rx = Values.WildcardToRegex(pattern, false);
                        int before = targets.Count;
                        foreach (Process pr in Process.GetProcesses())
                        {
                            string pn;
                            try { pn = pr.ProcessName; } catch (Exception) { continue; }
                            if (pn.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) pn = pn.Substring(0, pn.Length - 4);
                            if (rx.IsMatch(pn)) targets.Add(pr);
                        }
                        if (targets.Count == before && pattern.IndexOfAny(new[] { '*', '?' }) < 0)
                            inv.Interp.WriteError(new ErrorRecord(new RuntimeException("Cannot find a process with the name \"" + pattern + "\". Verify the process name and call the cmdlet again."), "NoProcessFoundForGivenName", ErrorCategory.ObjectNotFound, pattern), inv, inv.Line);
                    }
                    foreach (Process pr in targets)
                    {
                        try
                        {
                            Trace.Log("Stop-Process " + pr.Id);
                            if (!pr.HasExited) pr.Kill();
                            if (inv.GetSwitch("PassThru")) res.Add(pr);
                        }
                        catch (Exception ex)
                        {
                            inv.Interp.WriteError(new ErrorRecord(ex, "CouldNotStopProcess", ErrorCategory.CloseError, pr), inv, inv.Line);
                        }
                    }
                    return res;
                }));

            Add(new Cmdlet("Wait-Process")
                .Value("Name", 0).Value("Id", -1).Value("InputObject", -1).Value("Timeout", 1, "TimeoutSec")
                .Body(inv =>
                {
                    List<Process> targets = new List<Process>();
                    if (inv.Input != null) foreach (object o in inv.Input) { Process pr = Values.Unwrap(o) as Process; if (pr != null) targets.Add(pr); }
                    foreach (object o in inv.GetList("InputObject")) { Process pr = Values.Unwrap(o) as Process; if (pr != null) targets.Add(pr); }
                    foreach (object o in inv.GetList("Id")) { try { targets.Add(Process.GetProcessById(Values.ToInt(o, "Id"))); } catch (Exception) { } }
                    foreach (object o in inv.GetList("Name"))
                    {
                        string pattern = Values.ToPSString(o);
                        if (pattern.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) pattern = pattern.Substring(0, pattern.Length - 4);
                        System.Text.RegularExpressions.Regex rx = Values.WildcardToRegex(pattern, false);
                        foreach (Process pr in Process.GetProcesses())
                        {
                            string pn; try { pn = pr.ProcessName; } catch (Exception) { continue; }
                            if (pn.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) pn = pn.Substring(0, pn.Length - 4);
                            if (rx.IsMatch(pn)) targets.Add(pr);
                        }
                    }
                    int timeoutMs = inv.Has("Timeout") ? Values.ToInt(inv.Get("Timeout"), "Timeout") * 1000 : -1;
                    foreach (Process pr in targets)
                    {
                        try
                        {
                            bool exited = timeoutMs < 0 ? WaitAll(pr) : pr.WaitForExit(timeoutMs);
                            if (!exited) inv.Interp.WriteError(new ErrorRecord(new TimeoutException("This command stopped operation because process \"" + pr.ProcessName + "\" (" + pr.Id + ") has not exited within the timeout."), "ProcessNotTerminated", ErrorCategory.CloseError, pr), inv, inv.Line);
                        }
                        catch (Exception) { }
                    }
                    return None();
                }));

            Add(new Cmdlet("Start-Sleep", "sleep")
                .Value("Seconds", 0, "s").Value("Milliseconds", -1, "ms")
                .Body(inv =>
                {
                    double ms = 0;
                    if (inv.Has("Milliseconds")) ms = Values.ToInt(inv.Get("Milliseconds"), "Milliseconds");
                    else if (inv.Has("Seconds"))
                    {
                        double sec;
                        if (!Values.TryToNumber(inv.Get("Seconds"), out sec)) throw new ParameterBindingException("Start-Sleep : Cannot bind parameter 'Seconds'.");
                        ms = sec * 1000;
                    }
                    else throw new ParameterBindingException("Start-Sleep : Cannot process command because of one or more missing mandatory parameters: Seconds.");
                    Trace.Log("Start-Sleep " + ms + " ms");
                    if (ms > 0) Thread.Sleep((int)Math.Min(ms, int.MaxValue));
                    return None();
                }));

            /* ---- environment ---- */

            Add(new Cmdlet("Get-ExecutionPolicy")
                .Value("Scope", 0).Switch("List")
                .Body(inv => One("Unrestricted")));

            Add(new Cmdlet("Set-ExecutionPolicy")
                .Value("ExecutionPolicy", 0).Value("Scope", 1).Switch("Force")
                .Body(inv => None()));

            Add(new Cmdlet("Set-StrictMode")
                .Value("Version", -1).Switch("Off")
                .Body(inv => None()));

            Add(new Cmdlet("Get-Variable", "gv")
                .Value("Name", 0).Switch("ValueOnly")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    foreach (object o in inv.GetList("Name"))
                    {
                        object v = inv.Interp.GetVariable(Values.ToPSString(o));
                        res.Add(inv.GetSwitch("ValueOnly") ? v : (object)new KeyValuePair<string, object>(Values.ToPSString(o), v));
                    }
                    return res;
                }));

            Add(new Cmdlet("Set-Variable", "sv", "set")
                .Value("Name", 0).Required("Name").Value("Value", 1).Switch("Force").Value("Scope", -1).Value("Option", -1)
                .Body(inv => { inv.Interp.SetVariable(inv.GetString("Name"), inv.Get("Value")); return None(); }));

            Add(new Cmdlet("Get-Item", "gi")
                .Value("Path", 0, "LiteralPath").Required("Path").Switch("Force")
                .Body(inv =>
                {
                    List<object> res = new List<object>();
                    foreach (object o in inv.GetList("Path"))
                    {
                        string path = Values.ToPSString(o);
                        if (path.IndexOf(':') == 1 && path.Length > 2 && !Path.IsPathRooted(path))
                        {
                            inv.Interp.WriteError(new ErrorRecord(new RuntimeException("Provider drives such as '" + path.Substring(0, 2) + "' are not supported by this PowerShell subset."), "NotSupported", ErrorCategory.NotImplemented, path), inv, inv.Line);
                            continue;
                        }
                        if (Directory.Exists(path)) res.Add(new DirectoryInfo(path));
                        else if (File.Exists(path)) res.Add(new FileInfo(path));
                        else inv.Interp.WriteError(new ErrorRecord(new FileNotFoundException("Cannot find path '" + path + "' because it does not exist."), "PathNotFound", ErrorCategory.ObjectNotFound, path), inv, inv.Line);
                    }
                    return res;
                }));
        }

        private static bool WaitAll(Process pr) { pr.WaitForExit(); return true; }

        private static void CopyDirectory(string src, string dst, bool overwrite)
        {
            Directory.CreateDirectory(dst);
            foreach (string f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), overwrite);
            foreach (string d in Directory.GetDirectories(src)) CopyDirectory(d, Path.Combine(dst, Path.GetFileName(d)), overwrite);
        }

        /* Start-Process: CreateProcess semantics (UseShellExecute=false) unless
         * a verb is given; -WindowStyle maps to the STARTUPINFO show command,
         * -Wait blocks, -PassThru returns the Process. Failures are
         * non-terminating errors, as in Windows PowerShell. */
        private static List<object> StartProcess(Invocation inv)
        {
            string file = inv.GetString("FilePath");
            string args = Join(inv, "ArgumentList", " ");
            ProcessStartInfo psi = new ProcessStartInfo();
            string resolved = Interpreter.ResolveExecutable(file);
            psi.FileName = resolved ?? file;
            psi.Arguments = args;
            psi.UseShellExecute = false;
            if (inv.Has("Verb")) { psi.UseShellExecute = true; psi.Verb = inv.GetString("Verb"); }
            if (inv.Has("WorkingDirectory")) psi.WorkingDirectory = inv.GetString("WorkingDirectory");
            else psi.WorkingDirectory = Directory.GetCurrentDirectory();
            if (inv.Has("WindowStyle"))
            {
                string ws = inv.GetString("WindowStyle");
                if (ws.Equals("Hidden", StringComparison.OrdinalIgnoreCase)) { psi.WindowStyle = ProcessWindowStyle.Hidden; psi.CreateNoWindow = true; }
                else if (ws.Equals("Minimized", StringComparison.OrdinalIgnoreCase)) psi.WindowStyle = ProcessWindowStyle.Minimized;
                else if (ws.Equals("Maximized", StringComparison.OrdinalIgnoreCase)) psi.WindowStyle = ProcessWindowStyle.Maximized;
                else psi.WindowStyle = ProcessWindowStyle.Normal;
            }
            if (inv.Has("RedirectStandardOutput")) { psi.RedirectStandardOutput = true; psi.UseShellExecute = false; }
            if (inv.Has("RedirectStandardError")) { psi.RedirectStandardError = true; psi.UseShellExecute = false; }

            Trace.Log("Start-Process \"" + psi.FileName + "\" " + args + (inv.GetSwitch("Wait") ? " -Wait" : "") + (inv.GetSwitch("PassThru") ? " -PassThru" : ""));
            Process proc = new Process();
            proc.StartInfo = psi;
            try
            {
                if (!proc.Start()) throw new InvalidOperationException("Process.Start returned false.");
            }
            catch (Exception ex)
            {
                Trace.Log("Start-Process failed: " + ex.Message);
                inv.Interp.WriteError(new ErrorRecord(new InvalidOperationException("This command cannot be run due to the error: " + ex.Message, ex),
                    "InvalidOperationException,Microsoft.PowerShell.Commands.StartProcessCommand", ErrorCategory.InvalidOperation, file), inv, inv.Line);
                return None();
            }
            if (inv.Has("RedirectStandardOutput")) File.WriteAllText(inv.GetString("RedirectStandardOutput"), proc.StandardOutput.ReadToEnd());
            if (inv.Has("RedirectStandardError")) File.WriteAllText(inv.GetString("RedirectStandardError"), proc.StandardError.ReadToEnd());
            if (inv.GetSwitch("Wait"))
            {
                proc.WaitForExit();
                Trace.Log("Start-Process: exited " + proc.ExitCode);
            }
            return inv.GetSwitch("PassThru") ? One(proc) : None();
        }
    }
}
