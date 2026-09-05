/*
 * smahost.exe — minimal host for the System.Management.Automation shim.
 *
 * Drives the shim exactly the way Steinberg's Setup.exe does
 * (PowerShell.Create / AddScript / AddParameters / Invoke / Streams) and prints
 * every stream, so a script can be exercised without the installer.
 *
 *   wine smahost.exe <script.ps1> [arg ...]      run a script file
 *   wine smahost.exe -c "<code>" [arg ...]       run inline code
 *
 * Exit code: 0 when the error stream stayed empty, 100 otherwise (the
 * convention of Setup.exe's run_powershell), 101 on a terminating exception.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Management.Automation;

static class SmaHost
{
    static int Main(string[] argv)
    {
        if (argv.Length == 0) { Console.Error.WriteLine("usage: smahost.exe <script.ps1> [args] | -c <code> [args]"); return 2; }
        string script;
        int firstArg;
        if (argv[0] == "-c") { if (argv.Length < 2) { Console.Error.WriteLine("-c needs code"); return 2; } script = argv[1]; firstArg = 2; }
        else { script = File.ReadAllText(argv[0]); firstArg = 1; }

        int rc = 0;
        PowerShell ps = PowerShell.Create();
        try
        {
            ps.AddScript(script);
            if (argv.Length > firstArg)
            {
                List<string> rest = new List<string>();
                for (int i = firstArg; i < argv.Length; i++) rest.Add(argv[i]);
                ps.AddParameters(rest.ToArray());
            }
            Collection<PSObject> results = ps.Invoke();
            foreach (PSObject o in results) Console.WriteLine("OUTPUT: " + (o == null ? "$null" : o.ToString()));
            foreach (InformationRecord r in ps.Streams.Information) Console.WriteLine("INFO:   " + r.ToString());
            foreach (WarningRecord r in ps.Streams.Warning) Console.WriteLine("WARN:   " + r.ToString());
            foreach (ErrorRecord r in ps.Streams.Error) { Console.WriteLine("ERROR:  " + r.ToString() + "  [" + r.FullyQualifiedErrorId + "]"); rc = 100; }
            Console.WriteLine("EXIT:   " + ps.LastExitCode + "   HadErrors=" + ps.HadErrors + "  State=" + ps.InvocationStateInfo.State);
        }
        catch (Exception ex)
        {
            Console.WriteLine("EXCEPTION: " + ex.GetType().Name + ": " + ex.Message);
            rc = 101;
        }
        finally
        {
            ps.Dispose();
        }
        return rc;
    }
}
