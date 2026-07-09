using System;
using System.Drawing;
using System.Windows.Forms;

namespace WinFormsSample
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SampleForm());
        }
    }

    internal sealed class SampleForm : Form
    {
        public SampleForm()
        {
            Name = "MainForm";
            Text = "LVT WinForms Sample";
            StartPosition = FormStartPosition.Manual;
            Location = new Point(120, 120);
            Size = new Size(420, 260);

            var label = new Label
            {
                Name = "messageLabel",
                Text = "Sample label",
                AutoSize = true,
                Location = new Point(20, 20)
            };

            var textBox = new TextBox
            {
                Name = "inputTextBox",
                Text = "Sample text",
                Location = new Point(20, 55),
                Width = 220
            };

            var button = new Button
            {
                Name = "okButton",
                Text = "OK",
                Location = new Point(20, 95),
                Size = new Size(90, 30)
            };

            var checkBox = new CheckBox
            {
                Name = "enabledCheckBox",
                Text = "Enabled",
                Checked = true,
                Location = new Point(20, 135),
                AutoSize = true
            };

            Controls.Add(label);
            Controls.Add(textBox);
            Controls.Add(button);
            Controls.Add(checkBox);
        }
    }
}
