/*
 * System.Management.Automation shim for Wine-Mono.
 *
 * Windows PowerShell's hosting API is not part of Wine-Mono. .NET installers
 * that host PowerShell in-process (Steinberg Setup.exe: run_powershell) fail
 * to load the assembly, swallow the exception and finish without doing
 * anything. This assembly carries the reference name, version and public key
 * token of the real System.Management.Automation 3.0.0.0 and implements the
 * subset of the hosting API such installers use, on top of a small
 * interpreter for the PowerShell subset their prerun scripts need
 * (Interpreter.cs). Anything the interpreter does not understand is reported
 * as an error, never silently accepted.
 *
 * Only the public surface in this file is meant to be seen by the hosting
 * application. It mirrors the names and signatures of the real API so that
 * the IL references of the host resolve.
 */

using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Reflection;
using System.Runtime.InteropServices;

[assembly: AssemblyTitle("System.Management.Automation (Wine shim)")]
[assembly: AssemblyDescription("Subset of the Windows PowerShell hosting API for Wine-Mono (d2d1-dcomp branch)")]
[assembly: AssemblyProduct("mono-sma-shim (Wine d2d1-dcomp branch)")]
[assembly: AssemblyVersion("3.0.0.0")]
[assembly: AssemblyFileVersion("3.0.0.0")]
[assembly: AssemblyInformationalVersion("wine-shim 0.1")]
[assembly: ComVisible(false)]

namespace System.Management.Automation
{
    /* ------------------------------------------------------------------ */
    /* Exceptions                                                          */
    /* ------------------------------------------------------------------ */

    public class RuntimeException : SystemException
    {
        public RuntimeException() { }
        public RuntimeException(string message) : base(message) { }
        public RuntimeException(string message, Exception inner) : base(message, inner) { }
        public ErrorRecord ErrorRecord { get; internal set; }
    }

    public class ParseException : RuntimeException
    {
        public ParseException(string message) : base(message) { }
    }

    public class CommandNotFoundException : RuntimeException
    {
        public CommandNotFoundException(string message) : base(message) { }
        public string CommandName { get; internal set; }
    }

    public class ParameterBindingException : RuntimeException
    {
        public ParameterBindingException(string message) : base(message) { }
    }

    public class ActionPreferenceStopException : RuntimeException
    {
        public ActionPreferenceStopException(string message) : base(message) { }
    }

    /* Carried by Write-Error records that wrap no other exception. */
    public class WriteErrorException : Exception
    {
        public WriteErrorException(string message) : base(message) { }
    }

    /* ------------------------------------------------------------------ */
    /* Records                                                             */
    /* ------------------------------------------------------------------ */

    public enum ErrorCategory
    {
        NotSpecified = 0, OpenError = 1, CloseError = 2, DeviceError = 3,
        DeadlockDetected = 4, InvalidArgument = 5, InvalidData = 6,
        InvalidOperation = 7, InvalidResult = 8, InvalidType = 9,
        MetadataError = 10, NotImplemented = 11, NotInstalled = 12,
        ObjectNotFound = 13, OperationStopped = 14, OperationTimeout = 15,
        SyntaxError = 16, ParserError = 17, PermissionDenied = 18,
        ResourceBusy = 19, ResourceExists = 20, ResourceUnavailable = 21,
        ReadError = 22, WriteError = 23, FromStdErr = 24, SecurityError = 25,
        ProtocolError = 26, ConnectionError = 27, AuthenticationError = 28,
        LimitsExceeded = 29, QuotaExceeded = 30, NotEnabled = 31
    }

    public class ErrorCategoryInfo
    {
        public ErrorCategory Category { get; internal set; }
        public string Activity { get; internal set; }
        public string Reason { get; internal set; }
        public string TargetName { get; internal set; }
        public string TargetType { get; internal set; }
        public override string ToString()
        {
            return Category + ": (" + TargetName + ":" + TargetType + ") [" + Activity + "], " + Reason;
        }
    }

    public class ErrorDetails
    {
        public ErrorDetails(string message) { Message = message; }
        public string Message { get; private set; }
        public string RecommendedAction { get; set; }
        public override string ToString() { return Message; }
    }

    public class ErrorRecord
    {
        public ErrorRecord(Exception exception, string errorId, ErrorCategory errorCategory, object targetObject)
        {
            Exception = exception ?? new RuntimeException("Unknown error");
            FullyQualifiedErrorId = errorId ?? "";
            TargetObject = targetObject;
            CategoryInfo = new ErrorCategoryInfo
            {
                Category = errorCategory,
                Activity = "",
                Reason = Exception.GetType().Name,
                TargetName = targetObject == null ? "" : targetObject.ToString(),
                TargetType = targetObject == null ? "" : targetObject.GetType().Name
            };
        }

        public Exception Exception { get; private set; }
        public string FullyQualifiedErrorId { get; private set; }
        public object TargetObject { get; private set; }
        public ErrorCategoryInfo CategoryInfo { get; private set; }
        public ErrorDetails ErrorDetails { get; set; }
        public string ScriptStackTrace { get; internal set; }

        /* The real ErrorRecord prints the ErrorDetails message if present,
         * otherwise the exception message. Hosts log exactly this string. */
        public override string ToString()
        {
            if (ErrorDetails != null && !string.IsNullOrEmpty(ErrorDetails.Message))
                return ErrorDetails.Message;
            return Exception.Message;
        }
    }

    public abstract class InformationalRecord
    {
        protected InformationalRecord(string message) { Message = message ?? ""; }
        public string Message { get; internal set; }
        public override string ToString() { return Message; }
    }

    public class WarningRecord : InformationalRecord
    {
        public WarningRecord(string message) : base(message) { }
        public WarningRecord(string fullyQualifiedWarningId, string message) : base(message)
        {
            FullyQualifiedWarningId = fullyQualifiedWarningId;
        }
        public string FullyQualifiedWarningId { get; private set; }
    }

    public class VerboseRecord : InformationalRecord
    {
        public VerboseRecord(string message) : base(message) { }
    }

    public class DebugRecord : InformationalRecord
    {
        public DebugRecord(string message) : base(message) { }
    }

    /* Write-Host produces an InformationRecord whose MessageData is a
     * HostInformationMessage; ToString() of both yields the text. */
    public class HostInformationMessage
    {
        public string Message { get; set; }
        public bool? NoNewLine { get; set; }
        public override string ToString() { return Message ?? ""; }
    }

    public class InformationRecord
    {
        public InformationRecord(object messageData, string source)
        {
            MessageData = messageData;
            Source = source;
            TimeGenerated = DateTime.Now;
            Tags = new List<string>();
            User = Environment.UserName;
            Computer = Environment.MachineName;
            ProcessId = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
        }
        public object MessageData { get; private set; }
        public string Source { get; set; }
        public DateTime TimeGenerated { get; set; }
        public List<string> Tags { get; private set; }
        public string User { get; set; }
        public string Computer { get; set; }
        public uint ProcessId { get; set; }
        public override string ToString()
        {
            return MessageData == null ? "" : MessageData.ToString();
        }
    }

    public class ProgressRecord
    {
        public ProgressRecord(int activityId, string activity, string statusDescription)
        {
            ActivityId = activityId; Activity = activity; StatusDescription = statusDescription;
        }
        public int ActivityId { get; private set; }
        public string Activity { get; set; }
        public string StatusDescription { get; set; }
        public int PercentComplete { get; set; }
    }

    /* ------------------------------------------------------------------ */
    /* PSObject                                                            */
    /* ------------------------------------------------------------------ */

    public class PSObject
    {
        public PSObject() { BaseObject = null; }
        public PSObject(object obj)
        {
            PSObject other = obj as PSObject;
            BaseObject = other != null ? other.BaseObject : obj;
        }
        public object BaseObject { get; private set; }
        public object ImmediateBaseObject { get { return BaseObject; } }
        public static PSObject AsPSObject(object obj)
        {
            PSObject p = obj as PSObject;
            return p ?? new PSObject(obj);
        }
        public override string ToString()
        {
            return Shim.Values.ToPSString(BaseObject);
        }
        public override bool Equals(object obj)
        {
            PSObject o = obj as PSObject;
            object other = o != null ? o.BaseObject : obj;
            return object.Equals(BaseObject, other);
        }
        public override int GetHashCode()
        {
            return BaseObject == null ? 0 : BaseObject.GetHashCode();
        }
    }

    /* ------------------------------------------------------------------ */
    /* PSDataCollection<T> and the streams                                 */
    /* ------------------------------------------------------------------ */

    /* The real type is its own list implementation, not a Collection<T>;
     * hosts reference PSDataCollection`1::GetEnumerator() directly, so the
     * method has to be declared here and not merely inherited. */
    public class PSDataCollection<T> : IList<T>, ICollection<T>, IEnumerable<T>, IEnumerable, IDisposable
    {
        private readonly List<T> items = new List<T>();
        private bool isOpen = true;

        public PSDataCollection() { }
        public PSDataCollection(IEnumerable<T> initial) { if (initial != null) items.AddRange(initial); }

        public event EventHandler<DataAddedEventArgs> DataAdded;

        public bool IsOpen { get { return isOpen; } }
        public bool IsAutoGenerated { get; set; }
        public bool BlockingEnumerator { get; set; }
        public int Count { get { return items.Count; } }
        public bool IsReadOnly { get { return false; } }

        public T this[int index]
        {
            get { return items[index]; }
            set { items[index] = value; }
        }

        public void Add(T item)
        {
            items.Add(item);
            EventHandler<DataAddedEventArgs> h = DataAdded;
            if (h != null) h(this, new DataAddedEventArgs(Guid.Empty, items.Count - 1));
        }
        public void Clear() { items.Clear(); }
        public bool Contains(T item) { return items.Contains(item); }
        public void CopyTo(T[] array, int arrayIndex) { items.CopyTo(array, arrayIndex); }
        public int IndexOf(T item) { return items.IndexOf(item); }
        public void Insert(int index, T item) { items.Insert(index, item); }
        public bool Remove(T item) { return items.Remove(item); }
        public void RemoveAt(int index) { items.RemoveAt(index); }
        public void Complete() { isOpen = false; }
        public Collection<T> ReadAll()
        {
            Collection<T> c = new Collection<T>(new List<T>(items));
            items.Clear();
            return c;
        }
        public void Dispose() { Complete(); }

        public IEnumerator<T> GetEnumerator() { return items.GetEnumerator(); }
        IEnumerator IEnumerable.GetEnumerator() { return items.GetEnumerator(); }
    }

    public sealed class DataAddedEventArgs : EventArgs
    {
        internal DataAddedEventArgs(Guid psInstanceId, int index) { PowerShellInstanceId = psInstanceId; Index = index; }
        public Guid PowerShellInstanceId { get; private set; }
        public int Index { get; private set; }
    }

    public sealed class PSDataStreams
    {
        internal PSDataStreams()
        {
            Error = new PSDataCollection<ErrorRecord>();
            Warning = new PSDataCollection<WarningRecord>();
            Verbose = new PSDataCollection<VerboseRecord>();
            Debug = new PSDataCollection<DebugRecord>();
            Information = new PSDataCollection<InformationRecord>();
            Progress = new PSDataCollection<ProgressRecord>();
        }
        public PSDataCollection<ErrorRecord> Error { get; set; }
        public PSDataCollection<WarningRecord> Warning { get; set; }
        public PSDataCollection<VerboseRecord> Verbose { get; set; }
        public PSDataCollection<DebugRecord> Debug { get; set; }
        public PSDataCollection<InformationRecord> Information { get; set; }
        public PSDataCollection<ProgressRecord> Progress { get; set; }

        public void ClearStreams()
        {
            Error.Clear(); Warning.Clear(); Verbose.Clear(); Debug.Clear(); Information.Clear(); Progress.Clear();
        }
    }

    /* ------------------------------------------------------------------ */
    /* PowerShell                                                          */
    /* ------------------------------------------------------------------ */

    public enum PSInvocationState { NotStarted = 0, Running = 1, Stopping = 2, Stopped = 3, Completed = 4, Failed = 5, Disconnected = 6 }

    public sealed class PSInvocationStateInfo
    {
        internal PSInvocationStateInfo(PSInvocationState state, Exception reason) { State = state; Reason = reason; }
        public PSInvocationState State { get; private set; }
        public Exception Reason { get; private set; }
    }

    public sealed class PowerShell : IDisposable
    {
        internal sealed class Entry
        {
            public bool IsScript;
            public string Text;               /* script text or command name */
            public List<object> Positional = new List<object>();
            public List<KeyValuePair<string, object>> Named = new List<KeyValuePair<string, object>>();
        }

        private readonly List<Entry> entries = new List<Entry>();
        private readonly PSDataStreams streams = new PSDataStreams();
        private bool disposed;

        private PowerShell()
        {
            InstanceId = Guid.NewGuid();
            InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.NotStarted, null);
        }

        public static PowerShell Create() { return new PowerShell(); }
        public static PowerShell Create(object runspaceMode) { return new PowerShell(); }

        public Guid InstanceId { get; private set; }
        public PSDataStreams Streams { get { return streams; } }
        public bool HadErrors { get; private set; }
        public PSInvocationStateInfo InvocationStateInfo { get; private set; }
        public bool IsRunning { get; private set; }

        /* Shim extra: the value of the script's last `exit N`, 0 if none. The
         * real API exposes this only indirectly; hosts that want it usually
         * read $LASTEXITCODE. */
        public int LastExitCode { get; private set; }

        private Entry Current
        {
            get
            {
                if (entries.Count == 0) throw new InvalidOperationException("No command was added to the PowerShell instance.");
                return entries[entries.Count - 1];
            }
        }

        public PowerShell AddScript(string script) { return AddScript(script, false); }
        public PowerShell AddScript(string script, bool useLocalScope)
        {
            CheckDisposed();
            if (script == null) throw new ArgumentNullException("script");
            entries.Add(new Entry { IsScript = true, Text = script });
            return this;
        }

        public PowerShell AddCommand(string cmdlet) { return AddCommand(cmdlet, false); }
        public PowerShell AddCommand(string cmdlet, bool useLocalScope)
        {
            CheckDisposed();
            if (string.IsNullOrEmpty(cmdlet)) throw new ArgumentNullException("cmdlet");
            entries.Add(new Entry { IsScript = false, Text = cmdlet });
            return this;
        }

        public PowerShell AddStatement() { return this; }

        public PowerShell AddArgument(object value)
        {
            CheckDisposed();
            Current.Positional.Add(value);
            return this;
        }

        public PowerShell AddParameter(string parameterName)
        {
            CheckDisposed();
            Current.Named.Add(new KeyValuePair<string, object>(parameterName, true));
            return this;
        }

        public PowerShell AddParameter(string parameterName, object value)
        {
            CheckDisposed();
            Current.Named.Add(new KeyValuePair<string, object>(parameterName, value));
            return this;
        }

        /* Positional parameters: a script receives them as $args. */
        public PowerShell AddParameters(IList parameters)
        {
            CheckDisposed();
            if (parameters == null) throw new ArgumentNullException("parameters");
            foreach (object o in parameters) Current.Positional.Add(o);
            return this;
        }

        public PowerShell AddParameters(IDictionary parameters)
        {
            CheckDisposed();
            if (parameters == null) throw new ArgumentNullException("parameters");
            foreach (DictionaryEntry de in parameters)
                Current.Named.Add(new KeyValuePair<string, object>(de.Key.ToString(), de.Value));
            return this;
        }

        public Collection<PSObject> Invoke() { return Invoke(null); }

        public Collection<PSObject> Invoke(IEnumerable input)
        {
            CheckDisposed();
            Collection<PSObject> result = new Collection<PSObject>();
            List<object> inputList = null;
            if (input != null) { inputList = new List<object>(); foreach (object o in input) inputList.Add(o); }

            IsRunning = true;
            InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.Running, null);
            Shim.Trace.Log("PowerShell.Invoke: " + entries.Count + " entr" + (entries.Count == 1 ? "y" : "ies"));
            try
            {
                foreach (Entry e in entries)
                {
                    Shim.Interpreter interp = new Shim.Interpreter(streams);
                    List<object> output;
                    if (e.IsScript)
                        output = interp.RunScript(e.Text, e.Positional, e.Named, inputList);
                    else
                        output = interp.RunHostCommand(e.Text, e.Positional, e.Named, inputList);
                    foreach (object o in output) result.Add(PSObject.AsPSObject(o));
                    LastExitCode = interp.ExitCode;
                    if (interp.Exited) break;
                }
                InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.Completed, null);
            }
            catch (Exception ex)
            {
                InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.Failed, ex);
                HadErrors = true;
                Shim.Trace.Log("PowerShell.Invoke: terminating error: " + ex.GetType().Name + ": " + ex.Message);
                throw;
            }
            finally
            {
                IsRunning = false;
                if (streams.Error.Count > 0) HadErrors = true;
                Shim.Trace.Log("PowerShell.Invoke: done, output=" + result.Count + " errors=" + streams.Error.Count
                               + " warnings=" + streams.Warning.Count + " information=" + streams.Information.Count
                               + " exit=" + LastExitCode);
            }
            return result;
        }

        public void Stop() { }

        public void Dispose()
        {
            disposed = true;
            entries.Clear();
        }

        private void CheckDisposed()
        {
            if (disposed) throw new ObjectDisposedException("PowerShell");
        }
    }
}
