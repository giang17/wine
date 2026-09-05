/*
 * Value helpers and tracing for the PowerShell subset interpreter
 * (see Api.cs for the purpose of this assembly).
 */

using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace System.Management.Automation.Shim
{
    /* Optional trace log: set WINE_SMA_TRACE=<file> in the environment of the
     * hosting process to append one line per interpreter event. Off by
     * default; the shim never writes files on its own otherwise. */
    internal static class Trace
    {
        private static readonly string path = Environment.GetEnvironmentVariable("WINE_SMA_TRACE");
        private static readonly object gate = new object();

        public static bool Enabled { get { return !string.IsNullOrEmpty(path); } }

        public static void Log(string message)
        {
            if (!Enabled) return;
            try
            {
                lock (gate)
                {
                    File.AppendAllText(path, DateTime.Now.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture)
                                             + " [" + System.Diagnostics.Process.GetCurrentProcess().Id + "] "
                                             + message + Environment.NewLine);
                }
            }
            catch (Exception) { /* tracing must never break the host */ }
        }
    }

    internal static class Values
    {
        /* PowerShell string conversion: $null -> "", booleans capitalised,
         * numbers invariant, collections joined with a space. */
        public static string ToPSString(object v)
        {
            if (v == null) return "";
            PSObject pso = v as PSObject;
            if (pso != null) return ToPSString(pso.BaseObject);
            if (v is bool) return (bool)v ? "True" : "False";
            if (v is string) return (string)v;
            IFormattable f = v as IFormattable;
            if (f != null && IsNumber(v)) return f.ToString(null, CultureInfo.InvariantCulture);
            if (v is IEnumerable && !(v is IDictionary))
            {
                StringBuilder sb = new StringBuilder();
                foreach (object o in (IEnumerable)v)
                {
                    if (sb.Length > 0) sb.Append(' ');
                    sb.Append(ToPSString(o));
                }
                return sb.ToString();
            }
            System.Diagnostics.Process proc = v as System.Diagnostics.Process;
            if (proc != null)
            {
                string name;
                try { name = proc.ProcessName; } catch (Exception) { name = "?"; }
                return "System.Diagnostics.Process (" + name + ")";
            }
            return v.ToString();
        }

        public static bool IsNumber(object v)
        {
            return v is int || v is long || v is double || v is float || v is decimal
                || v is short || v is byte || v is uint || v is ulong || v is ushort || v is sbyte;
        }

        public static object Unwrap(object v)
        {
            PSObject pso = v as PSObject;
            return pso != null ? pso.BaseObject : v;
        }

        /* PowerShell truthiness. */
        public static bool ToBool(object v)
        {
            v = Unwrap(v);
            if (v == null) return false;
            if (v is bool) return (bool)v;
            if (v is string) return ((string)v).Length > 0;
            if (IsNumber(v)) return Convert.ToDouble(v, CultureInfo.InvariantCulture) != 0.0;
            ICollection c = v as ICollection;
            if (c != null)
            {
                if (c.Count == 0) return false;
                if (c.Count == 1) { foreach (object o in c) return ToBool(o); }
                return true;
            }
            return true;
        }

        public static bool TryToNumber(object v, out double d)
        {
            v = Unwrap(v);
            d = 0;
            if (v == null) return true;                 /* $null converts to 0 */
            if (v is bool) { d = (bool)v ? 1 : 0; return true; }
            if (IsNumber(v)) { d = Convert.ToDouble(v, CultureInfo.InvariantCulture); return true; }
            string s = v as string;
            if (s != null)
            {
                s = s.Trim();
                if (s.Length == 0) return false;
                if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                {
                    long l;
                    if (long.TryParse(s.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out l)) { d = l; return true; }
                    return false;
                }
                return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out d);
            }
            return false;
        }

        public static object Normalize(double d)
        {
            if (d == Math.Floor(d) && Math.Abs(d) < int.MaxValue) return (int)d;
            if (d == Math.Floor(d) && Math.Abs(d) < long.MaxValue) return (long)d;
            return d;
        }

        public static int ToInt(object v, string what)
        {
            double d;
            if (!TryToNumber(v, out d))
                throw new RuntimeException("Cannot convert value \"" + ToPSString(v) + "\" to type \"System.Int32\" (" + what + ").");
            return (int)d;
        }

        /* -eq and friends: if the left operand is numeric the right one is
         * converted to a number, otherwise both are compared as strings,
         * case-insensitively unless asked otherwise. */
        public static int Compare(object l, object r, bool caseSensitive)
        {
            l = Unwrap(l); r = Unwrap(r);
            if (l == null && r == null) return 0;
            if (l is bool) return ((bool)l).CompareTo(ToBool(r));
            if (IsNumber(l) || (l == null && IsNumber(r)))
            {
                double dl, dr;
                if (TryToNumber(l, out dl) && TryToNumber(r, out dr)) return dl.CompareTo(dr);
                if (l == null) return -1;
            }
            if (l == null) return r == null || (r is string && ((string)r).Length == 0) ? 0 : -1;
            if (r == null) return l is string && ((string)l).Length == 0 ? 0 : 1;
            string sl = ToPSString(l), sr = ToPSString(r);
            return string.Compare(sl, sr, caseSensitive ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase);
        }

        public static bool AreEqual(object l, object r, bool caseSensitive)
        {
            l = Unwrap(l); r = Unwrap(r);
            if (l == null) return r == null || (r is string && ((string)r).Length == 0);
            if (l is bool) return (bool)l == ToBool(r);
            if (IsNumber(l))
            {
                double dl, dr;
                if (TryToNumber(l, out dl) && TryToNumber(r, out dr)) return dl == dr;
                return false;
            }
            return string.Equals(ToPSString(l), ToPSString(r), caseSensitive ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase);
        }

        /* Wildcard pattern (-like, Get-Process -Name) to a regular expression. */
        public static System.Text.RegularExpressions.Regex WildcardToRegex(string pattern, bool caseSensitive)
        {
            StringBuilder sb = new StringBuilder("^");
            foreach (char c in pattern)
            {
                switch (c)
                {
                    case '*': sb.Append(".*"); break;
                    case '?': sb.Append('.'); break;
                    default: sb.Append(System.Text.RegularExpressions.Regex.Escape(c.ToString())); break;
                }
            }
            sb.Append('$');
            System.Text.RegularExpressions.RegexOptions o = System.Text.RegularExpressions.RegexOptions.Singleline;
            if (!caseSensitive) o |= System.Text.RegularExpressions.RegexOptions.IgnoreCase;
            return new System.Text.RegularExpressions.Regex(sb.ToString(), o);
        }

        public static List<object> ToList(object v)
        {
            List<object> list = new List<object>();
            v = Unwrap(v);
            if (v == null) return list;
            if (v is string) { list.Add(v); return list; }
            IEnumerable e = v as IEnumerable;
            if (e != null && !(v is IDictionary)) { foreach (object o in e) list.Add(Unwrap(o)); return list; }
            list.Add(v);
            return list;
        }

        /* A pipeline used as a value: nothing -> $null, one item -> the item,
         * more -> object[]. */
        public static object FromList(List<object> list)
        {
            if (list == null || list.Count == 0) return null;
            if (list.Count == 1) return list[0];
            return list.ToArray();
        }
    }
}
