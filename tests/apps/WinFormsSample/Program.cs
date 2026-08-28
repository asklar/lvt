using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Linq;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
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
            TypeDescriptor.AddProvider(
                new SampleFormTypeDescriptionProvider(
                    TypeDescriptor.GetProvider(typeof(SampleForm))),
                typeof(SampleForm));
            Application.Run(new SampleForm());
        }
    }

    internal enum SampleMode
    {
        Basic,
        Advanced,
        Expert,
    }

    internal sealed class SampleForm : Form
    {
        private readonly EventWaitHandle blockTrigger;
        private readonly EventWaitHandle blockEntered;
        private readonly EventWaitHandle blockRelease;
        private string editableText = "Default text";
        private int retryCount = 5;
        private SampleMode mode = SampleMode.Basic;

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

            var prefix = $@"Local\LvtWinFormsSampleUiBlock_{Process.GetCurrentProcess().Id}";
            blockTrigger = new EventWaitHandle(
                false, EventResetMode.AutoReset, prefix + "_trigger");
            blockEntered = new EventWaitHandle(
                false, EventResetMode.ManualReset, prefix + "_entered");
            blockRelease = new EventWaitHandle(
                false, EventResetMode.AutoReset, prefix + "_release");
            _ = Task.Run(() =>
            {
                while (blockTrigger.WaitOne())
                {
                    BeginInvoke(new MethodInvoker(() =>
                    {
                        blockEntered.Set();
                        blockRelease.WaitOne();
                        blockEntered.Reset();
                    }));
                }
            });
        }

        [Browsable(true)]
        [DefaultValue("Default text")]
        public string EditableText
        {
            get => editableText;
            set => editableText = value;
        }

        [Browsable(true)]
        [DefaultValue(5)]
        public int RetryCount
        {
            get => retryCount;
            set => retryCount = value;
        }

        [Browsable(true)]
        [DefaultValue(SampleMode.Basic)]
        public SampleMode Mode
        {
            get => mode;
            set => mode = value;
        }

        [Browsable(true)]
        public string ReadOnlyValue => "read only";

        [Browsable(true)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        public Font UnsafeFontProperty { get; set; } = SystemFonts.DefaultFont;

        [Browsable(true)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        public string ThrowingValue
        {
            get => "unchanged";
            set => throw new InvalidOperationException("Sample setter rejected the value");
        }

        [Browsable(false)]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
        internal string ProviderTextValue { get; set; } = "Provider default";
    }

    internal sealed class SampleFormTypeDescriptionProvider : TypeDescriptionProvider
    {
        private readonly TypeDescriptionProvider baseProvider;

        public SampleFormTypeDescriptionProvider(TypeDescriptionProvider baseProvider)
        {
            this.baseProvider = baseProvider;
        }

        public override ICustomTypeDescriptor GetTypeDescriptor(
            Type objectType, object instance)
        {
            return new SampleFormTypeDescriptor(
                baseProvider.GetTypeDescriptor(objectType, instance));
        }
    }

    internal sealed class SampleFormTypeDescriptor : CustomTypeDescriptor
    {
        public SampleFormTypeDescriptor(ICustomTypeDescriptor parent)
            : base(parent)
        {
        }

        public override PropertyDescriptorCollection GetProperties()
        {
            return GetProperties(Array.Empty<Attribute>());
        }

        public override PropertyDescriptorCollection GetProperties(Attribute[] attributes)
        {
            var properties = base.GetProperties(attributes)
                .Cast<PropertyDescriptor>()
                .ToList();
            properties.Add(new ProviderTextPropertyDescriptor());
            return new PropertyDescriptorCollection(properties.ToArray(), true);
        }
    }

    internal sealed class ProviderTextPropertyDescriptor : PropertyDescriptor
    {
        public ProviderTextPropertyDescriptor()
            : base(
                "ProviderText",
                new Attribute[]
                {
                    BrowsableAttribute.Yes,
                    new DefaultValueAttribute("Provider default"),
                })
        {
        }

        public override Type ComponentType => typeof(SampleForm);
        public override bool IsReadOnly => false;
        public override Type PropertyType => typeof(string);

        public override bool CanResetValue(object component)
        {
            return ((SampleForm)component).ProviderTextValue != "Provider default";
        }

        public override object GetValue(object component)
        {
            return ((SampleForm)component).ProviderTextValue;
        }

        public override void ResetValue(object component)
        {
            ((SampleForm)component).ProviderTextValue = "Provider default";
            OnValueChanged(component, EventArgs.Empty);
        }

        public override void SetValue(object component, object value)
        {
            ((SampleForm)component).ProviderTextValue = (string)value;
            OnValueChanged(component, EventArgs.Empty);
        }

        public override bool ShouldSerializeValue(object component)
        {
            return CanResetValue(component);
        }
    }
}
