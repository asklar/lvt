using System;
using System.IO.Pipes;
using System.Text;
using System.Windows.Forms;

namespace LvtWinFormsTap
{
    public static class WinFormsTreeWalker
    {
        public delegate int CollectTreeDelegate(IntPtr pipeNamePtr, int pipeNameLength);

        public static int CollectTree(IntPtr pipeNamePtr, int pipeNameLength)
        {
            try
            {
                string pipeName = System.Runtime.InteropServices.Marshal.PtrToStringUni(pipeNamePtr);
                return CollectTreeImpl(pipeName);
            }
            catch
            {
                return -1;
            }
        }

        public static int CollectTree(string pipeName)
        {
            return CollectTreeImpl(pipeName);
        }

        private static int CollectTreeImpl(string pipeName)
        {
            try
            {
                string json = null;
                if (Application.OpenForms.Count > 0 && Application.OpenForms[0].IsHandleCreated)
                {
                    var first = Application.OpenForms[0];
                    first.BeginInvoke(new MethodInvoker(() => json = WalkAllForms()));

                    var deadline = DateTime.UtcNow.AddSeconds(5);
                    while (json == null && DateTime.UtcNow < deadline)
                        System.Threading.Thread.Sleep(10);
                }

                if (json == null)
                    json = WalkAllForms();

                string shortName = pipeName;
                const string pipePrefix = @"\\.\pipe\";
                if (shortName.StartsWith(pipePrefix))
                    shortName = shortName.Substring(pipePrefix.Length);

                using (var client = new NamedPipeClientStream(".", shortName, PipeDirection.Out))
                {
                    client.Connect(5000);
                    byte[] data = Encoding.UTF8.GetBytes(json);
                    client.Write(data, 0, data.Length);
                    client.Flush();
                }

                return 0;
            }
            catch
            {
                return -1;
            }
        }

        private static string WalkAllForms()
        {
            var sb = new StringBuilder();
            sb.Append('[');
            bool first = true;
            foreach (Form form in Application.OpenForms)
            {
                if (!first) sb.Append(',');
                first = false;
                SerializeControl(sb, form);
            }
            sb.Append(']');
            return sb.ToString();
        }

        private static void SerializeControl(StringBuilder sb, Control control)
        {
            sb.Append('{');
            sb.Append("\"hwnd\":\"").Append(((long)control.Handle).ToString("X")).Append('"');
            sb.Append(",\"type\":\"").Append(EscapeJson(control.GetType().FullName ?? control.GetType().Name)).Append('"');

            if (!string.IsNullOrEmpty(control.Name))
                sb.Append(",\"name\":\"").Append(EscapeJson(control.Name)).Append('"');
            if (!string.IsNullOrEmpty(control.Text))
                sb.Append(",\"text\":\"").Append(EscapeJson(Trim(control.Text))).Append('"');
            if (!control.Visible)
                sb.Append(",\"visible\":false");
            if (!control.Enabled)
                sb.Append(",\"enabled\":false");
            if (control is TextBoxBase textBox)
                sb.Append(",\"readOnly\":").Append(textBox.ReadOnly ? "true" : "false");
            if (control is ButtonBase button)
                sb.Append(",\"autoSize\":").Append(button.AutoSize ? "true" : "false");

            if (control.Controls.Count > 0)
            {
                sb.Append(",\"children\":[");
                for (int i = 0; i < control.Controls.Count; i++)
                {
                    if (i > 0) sb.Append(',');
                    SerializeControl(sb, control.Controls[i]);
                }
                sb.Append(']');
            }

            sb.Append('}');
        }

        private static string Trim(string value)
        {
            return value.Length > 200 ? value.Substring(0, 200) : value;
        }

        private static string EscapeJson(string s)
        {
            if (s == null) return "";
            var sb = new StringBuilder(s.Length);
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20)
                            sb.AppendFormat("\\u{0:X4}", (int)c);
                        else
                            sb.Append(c);
                        break;
                }
            }
            return sb.ToString();
        }
    }
}
