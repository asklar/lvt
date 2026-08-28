using System;
using System.Collections.Generic;
using System.IO.Pipes;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace LvtWinFormsTap
{
    public static class WinFormsTreeWalker
    {
        public delegate int RunServerDelegate(IntPtr pipeNamePtr, int pipeNameLength);

        private const int UiTimeoutMilliseconds = 10000;
        private static readonly string AssemblyInstanceId = Guid.NewGuid().ToString("N");
        private static readonly ConditionalWeakTable<Control, Identity> Identities =
            new ConditionalWeakTable<Control, Identity>();
        private static long nextIdentity;
        private static int serverStartCount;

        private delegate bool EnumWindowsCallback(IntPtr hwnd, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(
            EnumWindowsCallback callback, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(
            IntPtr hwnd, out uint processId);

        private sealed class Identity
        {
            public Identity(long value)
            {
                Value = value;
            }

            public long Value { get; private set; }
        }

        private sealed class ObjectRegistry
        {
            private Dictionary<long, WeakReference> objects =
                new Dictionary<long, WeakReference>();

            public Dictionary<long, WeakReference> CreateSnapshot()
            {
                return new Dictionary<long, WeakReference>();
            }

            public long Track(Control value, Dictionary<long, WeakReference> snapshot)
            {
                Identity identity = Identities.GetValue(
                    value, ignored => new Identity(Interlocked.Increment(ref nextIdentity)));
                snapshot[identity.Value] = new WeakReference(value);
                return identity.Value;
            }

            public void Commit(Dictionary<long, WeakReference> snapshot)
            {
                objects = snapshot;
            }

            public bool TryGet(long handle, out Control value)
            {
                WeakReference reference;
                value = null;
                if (!objects.TryGetValue(handle, out reference))
                    return false;
                value = reference.Target as Control;
                return value != null;
            }

            public void Clear()
            {
                objects.Clear();
            }
        }

        public static int RunServer(IntPtr pipeNamePtr, int pipeNameLength)
        {
            try
            {
                string pipeName = Marshal.PtrToStringUni(pipeNamePtr);
                return RunServerImpl(pipeName);
            }
            catch
            {
                return -1;
            }
        }

        public static int RunServer(string pipeName)
        {
            return RunServerImpl(pipeName);
        }

        private static int RunServerImpl(string pipeName)
        {
            if (string.IsNullOrEmpty(pipeName))
                return 1;

            string shortName = pipeName;
            const string pipePrefix = @"\\.\pipe\";
            if (shortName.StartsWith(pipePrefix, StringComparison.OrdinalIgnoreCase))
                shortName = shortName.Substring(pipePrefix.Length);

            var registry = new ObjectRegistry();
            try
            {
                using (var client = new NamedPipeClientStream(
                    ".", shortName, PipeDirection.InOut, PipeOptions.None))
                {
                    client.Connect(5000);
                    using (var reader = new System.IO.StreamReader(
                        client, new UTF8Encoding(false), false, 4096, true))
                    using (var writer = new System.IO.StreamWriter(
                        client, new UTF8Encoding(false), 4096, true))
                    {
                        writer.AutoFlush = true;
                        int startCount = Interlocked.Increment(ref serverStartCount);
                        string connectionId = Guid.NewGuid().ToString("N");
                        writer.WriteLine(
                            "READY\t{\"protocol\":1,\"connectionId\":\"" + connectionId +
                            "\",\"assemblyInstanceId\":\"" + AssemblyInstanceId +
                            "\",\"serverStartCount\":" + startCount +
                            ",\"commands\":[\"GET_TREE\",\"DISCONNECT\"]}");

                        for (;;)
                        {
                            string line = reader.ReadLine();
                            if (line == null)
                                break;

                            string[] parts = line.Split(new[] { '\t' }, 4);
                            if (parts.Length < 3 || parts[0] != "REQUEST")
                                continue;

                            string commandId = parts[1];
                            string command = parts[2];
                            if (command == "GET_TREE")
                            {
                                try
                                {
                                    string tree = CollectTreeOnUiThread(registry);
                                    WriteResponse(writer, commandId, "OK", tree);
                                }
                                catch (Exception error)
                                {
                                    WriteResponse(
                                        writer, commandId, "ERROR",
                                        "{\"message\":\"" + EscapeJson(error.Message) + "\"}");
                                }
                            }
                            else if (command == "DISCONNECT")
                            {
                                WriteResponse(writer, commandId, "OK", "{}");
                                break;
                            }
                            else
                            {
                                WriteResponse(
                                    writer, commandId, "ERROR",
                                    "{\"message\":\"unsupported command\"}");
                            }
                        }
                    }
                }
                return 0;
            }
            catch
            {
                return -1;
            }
            finally
            {
                registry.Clear();
            }
        }

        private static void WriteResponse(
            System.IO.StreamWriter writer, string commandId, string status, string payload)
        {
            writer.WriteLine("RESPONSE\t" + commandId + "\t" + status + "\t" + payload);
        }

        private static string CollectTreeOnUiThread(ObjectRegistry registry)
        {
            Control marshalControl = FindMarshalControl();
            if (marshalControl == null)
                throw new InvalidOperationException(
                    "No WinForms UI control with a created handle is available");

            Dictionary<long, WeakReference> snapshot = registry.CreateSnapshot();
            var completion = new TaskCompletionSource<string>();
            marshalControl.BeginInvoke(new MethodInvoker(() =>
            {
                try
                {
                    completion.SetResult(WalkAllForms(registry, snapshot));
                }
                catch (Exception error)
                {
                    completion.SetException(error);
                }
            }));

            if (!completion.Task.Wait(UiTimeoutMilliseconds))
                throw new TimeoutException("WinForms UI thread did not complete the tree walk");
            string tree = completion.Task.GetAwaiter().GetResult();
            registry.Commit(snapshot);
            return tree;
        }

        private static Control FindMarshalControl()
        {
            Control result = null;
            uint currentProcessId = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
            EnumWindows((hwnd, parameter) =>
            {
                uint windowProcessId;
                GetWindowThreadProcessId(hwnd, out windowProcessId);
                if (windowProcessId != currentProcessId)
                    return true;

                Control control = Control.FromHandle(hwnd);
                if (control == null)
                    return true;
                result = control;
                return false;
            }, IntPtr.Zero);
            return result;
        }

        private static string WalkAllForms(
            ObjectRegistry registry, Dictionary<long, WeakReference> snapshot)
        {
            var builder = new StringBuilder();
            builder.Append('[');
            bool first = true;
            foreach (Form form in Application.OpenForms)
            {
                if (form == null)
                    continue;
                if (!first)
                    builder.Append(',');
                first = false;
                SerializeControl(builder, form, registry, snapshot);
            }
            builder.Append(']');
            return builder.ToString();
        }

        private static void SerializeControl(
            StringBuilder builder, Control control, ObjectRegistry registry,
            Dictionary<long, WeakReference> snapshot)
        {
            long managedHandle = registry.Track(control, snapshot);
            builder.Append('{');
            builder.Append("\"managedHandle\":").Append(managedHandle);
            if (control.IsHandleCreated)
            {
                builder.Append(",\"hwnd\":\"").Append(
                    control.Handle.ToInt64().ToString("X")).Append('"');
            }
            builder.Append(",\"type\":\"").Append(
                EscapeJson(control.GetType().FullName ?? control.GetType().Name)).Append('"');

            if (!string.IsNullOrEmpty(control.Name))
                builder.Append(",\"name\":\"").Append(EscapeJson(control.Name)).Append('"');
            if (!string.IsNullOrEmpty(control.Text))
                builder.Append(",\"text\":\"").Append(EscapeJson(Trim(control.Text))).Append('"');
            if (!control.Visible)
                builder.Append(",\"visible\":false");
            if (!control.Enabled)
                builder.Append(",\"enabled\":false");
            if (control is TextBoxBase textBox)
                builder.Append(",\"readOnly\":").Append(textBox.ReadOnly ? "true" : "false");
            if (control is ButtonBase button)
                builder.Append(",\"autoSize\":").Append(button.AutoSize ? "true" : "false");

            if (control.Controls.Count > 0)
            {
                builder.Append(",\"children\":[");
                for (int index = 0; index < control.Controls.Count; index++)
                {
                    if (index > 0)
                        builder.Append(',');
                    SerializeControl(builder, control.Controls[index], registry, snapshot);
                }
                builder.Append(']');
            }
            builder.Append('}');
        }

        private static string Trim(string value)
        {
            return value.Length > 200 ? value.Substring(0, 200) : value;
        }

        private static string EscapeJson(string value)
        {
            if (value == null)
                return "";
            var builder = new StringBuilder(value.Length);
            foreach (char character in value)
            {
                switch (character)
                {
                    case '"': builder.Append("\\\""); break;
                    case '\\': builder.Append("\\\\"); break;
                    case '\n': builder.Append("\\n"); break;
                    case '\r': builder.Append("\\r"); break;
                    case '\t': builder.Append("\\t"); break;
                    default:
                        if (character < 0x20)
                            builder.AppendFormat("\\u{0:X4}", (int)character);
                        else
                            builder.Append(character);
                        break;
                }
            }
            return builder.ToString();
        }
    }
}
