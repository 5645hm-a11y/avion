// A player whose every control is yours, with AVION playing inside it (API 12 embedding). The video panel's HWND is given to the
// engine; the bar below is ordinary WinForms, drawn here. Commands go out through AvionEngine.Command, and what happened comes
// back as events, polled 60 times a second.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Avion;

namespace AvionPlayer;

sealed class PlayerForm : Form
{
    static readonly Color Paper = Color.FromArgb(0x12, 0x12, 0x11), Bar = Color.FromArgb(0x1B, 0x1A, 0x18), Ink = Color.FromArgb(0xF2, 0xEF, 0xE9),
                          Muted = Color.FromArgb(0x9C, 0x99, 0x8F), Rule = Color.FromArgb(0x3A, 0x39, 0x35), Clay = Color.FromArgb(0xD9, 0x77, 0x57);
    static readonly string IconFont = FontFamilyExists("Segoe Fluent Icons") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";

    readonly AvionEngine engine = new();
    readonly Panel video = new() { Dock = DockStyle.Fill, BackColor = Color.Black };
    readonly Panel bar = new() { Dock = DockStyle.Bottom, Height = 100, BackColor = Bar, Padding = new Padding(18, 10, 18, 10) };
    readonly Slider seek = new() { Dock = DockStyle.Top, Height = 22 };
    readonly IconButton play = new("", 46) { Accent = true }, back = new("", 38), forward = new("", 38),
                        mute = new("", 38), full = new("", 38), open = new("", 38);
    readonly Slider volume = new() { Width = 110, Height = 22, Value = 1 };
    readonly Label time = new() { AutoSize = true, ForeColor = Ink, Font = new Font("Segoe UI Variable Text", 11f, FontStyle.Regular), Text = "0:00 / 0:00" };
    readonly Label title = new() { AutoSize = true, ForeColor = Muted, Font = new Font("Segoe UI Variable Text", 10f), Text = "Open a file, or drop one here" };
    readonly System.Windows.Forms.Timer timer = new() { Interval = 16 };
    long duration;
    bool paused, muted;
    FormWindowState restoreState;
    Rectangle restoreBounds;

    readonly bool startMuted;
    readonly string? startAt, capture;
    DateTime captureAt = DateTime.MaxValue;
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);

    public PlayerForm(string? path, bool startMuted = false, string? startAt = null, string? capture = null)
    {
        this.startMuted = startMuted; this.startAt = startAt; this.capture = capture;
        Text = "AVION Player — an embedding sample"; BackColor = Paper; ClientSize = new Size(1120, 700); MinimumSize = new Size(560, 360);
        StartPosition = FormStartPosition.CenterScreen; KeyPreview = true; AllowDrop = true; DoubleBuffered = true;
        if (capture != null) { StartPosition = FormStartPosition.Manual; Location = new Point(-4000, -4000); ShowInTaskbar = false; } // Off the screens.
        var row = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, BackColor = Bar, Padding = new Padding(0, 6, 0, 0) };
        var right = new FlowLayoutPanel { Dock = DockStyle.Right, AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, BackColor = Bar, Padding = new Padding(0, 6, 0, 0) };
        foreach (Control c in new Control[] { back, play, forward, Spacer(14), time, Spacer(18), title }) row.Controls.Add(c);
        foreach (Control c in new Control[] { mute, volume, Spacer(10), full, open }) right.Controls.Add(c);
        time.Margin = title.Margin = new Padding(0, 13, 0, 0); volume.Margin = new Padding(4, 14, 0, 0);
        bar.Controls.Add(row); bar.Controls.Add(right); bar.Controls.Add(seek);
        Controls.Add(video); Controls.Add(bar);

        play.Click += (_, _) => engine.Command(paused ? "play" : "pause");
        back.Click += (_, _) => engine.Command("back 10 seconds");
        forward.Click += (_, _) => engine.Command("forward 10 seconds");
        mute.Click += (_, _) => engine.Command(muted ? "unmute" : "mute");
        full.Click += (_, _) => ToggleFullscreen();
        open.Click += (_, _) => { using var d = new OpenFileDialog { Filter = "Video|*.mp4;*.m4v;*.mov|All files|*.*" }; if (d.ShowDialog(this) == DialogResult.OK) Open(d.FileName); };
        seek.Released += v => { if (duration > 0) engine.Command("go to " + Clock((long)(v * duration))); };
        seek.Dragging += v => time.Text = Clock((long)(v * duration)) + " / " + Clock(duration);
        volume.Released += v => engine.Command("volume " + (int)Math.Round(v * 100));
        DragEnter += (_, e) => e.Effect = e.Data?.GetDataPresent(DataFormats.FileDrop) == true ? DragDropEffects.Copy : DragDropEffects.None;
        DragDrop += (_, e) => { if (e.Data?.GetData(DataFormats.FileDrop) is string[] files && files.Length > 0) Open(files[0]); };
        KeyDown += (_, e) =>
        {
            if (e.KeyCode == Keys.Space) { engine.Command(paused ? "play" : "pause"); e.Handled = true; }
            else if (e.KeyCode == Keys.Escape && FormBorderStyle == FormBorderStyle.None) ToggleFullscreen();
        };
        timer.Tick += (_, _) => Tick();
        Shown += (_, _) => { timer.Start(); if (path != null) Open(path); };
        FormClosing += (_, _) => { timer.Stop(); engine.Dispose(); };
    }

    void Open(string path)
    {
        title.Text = Path.GetFileName(path);
        engine.Play(path, video.Handle);
        if (!engine.Playing) title.Text = "Could not open " + Path.GetFileName(path);
        else
        {
            if (startMuted) engine.Command("mute");
            if (startAt != null) engine.Command("go to " + startAt);
        }
    }

    void Tick()
    {
        // Every event since the last tick, then the clock from the published state (any thread may read it).
        while (engine.Poll() is { } e)
        {
            switch (e.Kind)
            {
            case Native.EventKind.Opened: duration = e.Duration; if (capture != null) captureAt = DateTime.Now.AddSeconds(4); break;
            case Native.EventKind.State:
                paused = e.Paused != 0; muted = e.Muted != 0;
                play.Glyph = paused ? "" : ""; mute.Glyph = muted ? "" : "";
                volume.Value = e.Volume; break;
            case Native.EventKind.FullscreenRequest: ToggleFullscreen(); break; // F, F11, Alt+Enter on the picture.
            case Native.EventKind.Ended: engine.Stop(); paused = true; play.Glyph = ""; break;
            }
        }
        if (DateTime.Now >= captureAt) { captureAt = DateTime.MaxValue; SaveCapture(); Close(); return; }
        if (engine.State() is { } s && !seek.Held)
        {
            duration = s.Duration;
            seek.Value = duration > 0 ? (float)s.Position / duration : 0;
            time.Text = Clock(s.Position) + " / " + Clock(duration);
        }
    }

    // --capture: the window's own pixels (the engine's picture and this bar), wherever it is, as a PNG.
    void SaveCapture()
    {
        using var bmp = new Bitmap(Width, Height);
        using (var g = Graphics.FromImage(bmp)) { var dc = g.GetHdc(); PrintWindow(Handle, dc, 2); g.ReleaseHdc(dc); }
        bmp.Save(capture!, System.Drawing.Imaging.ImageFormat.Png);
    }

    void ToggleFullscreen()
    {
        if (FormBorderStyle != FormBorderStyle.None)
        {
            restoreState = WindowState; restoreBounds = Bounds;
            FormBorderStyle = FormBorderStyle.None; WindowState = FormWindowState.Normal; Bounds = Screen.FromControl(this).Bounds;
            full.Glyph = "";
        }
        else
        {
            FormBorderStyle = FormBorderStyle.Sizable; Bounds = restoreBounds; WindowState = restoreState;
            full.Glyph = "";
        }
    }

    static string Clock(long t100ns)
    {
        var t = TimeSpan.FromTicks(Math.Max(0, t100ns));
        return t.TotalHours >= 1 ? $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}" : $"{t.Minutes}:{t.Seconds:00}";
    }
    static Control Spacer(int width) => new Panel { Width = width, Height = 1, BackColor = Bar };
    static bool FontFamilyExists(string name) { using var f = new Font(name, 10f); return f.Name == name; }

    // A round icon button (Segoe Fluent Icons), clay when it is the main action.
    sealed class IconButton : Control
    {
        string glyph; bool hover;
        public bool Accent { get; init; }
        public string Glyph { get => glyph; set { glyph = value; Invalidate(); } }
        public IconButton(string glyph, int size)
        {
            this.glyph = glyph; Size = new Size(size, size); Margin = new Padding(3, 0, 3, 0); Cursor = Cursors.Hand;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Bar;
        }
        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            using (var fill = new SolidBrush(Accent ? (hover ? Color.FromArgb(0xE5, 0x8A, 0x6C) : Clay) : hover ? Rule : Bar)) g.FillEllipse(fill, r);
            using var font = new Font(IconFont, Accent ? 15f : 13f);
            TextRenderer.DrawText(g, glyph, font, r, Accent ? Color.FromArgb(0x14, 0x14, 0x13) : Ink, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
    }

    // A slider: a rounded track, clay progress, a thumb on hover or drag. Released(v) when let go; Dragging(v) while held.
    sealed class Slider : Control
    {
        float value; bool hover;
        public bool Held { get; private set; }
        public event Action<float>? Released, Dragging;
        public float Value { get => value; set { if (!Held) { this.value = Math.Clamp(value, 0, 1); Invalidate(); } } }
        public Slider()
        {
            Cursor = Cursors.Hand;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
            BackColor = Bar;
        }
        float At(int x) => Math.Clamp((x - 8f) / Math.Max(1, Width - 16f), 0, 1);
        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); }
        protected override void OnMouseDown(MouseEventArgs e) { Held = true; value = At(e.X); Dragging?.Invoke(value); Invalidate(); }
        protected override void OnMouseMove(MouseEventArgs e) { if (Held) { value = At(e.X); Dragging?.Invoke(value); Invalidate(); } }
        protected override void OnMouseUp(MouseEventArgs e) { if (!Held) return; Held = false; value = At(e.X); Released?.Invoke(value); Invalidate(); }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias;
            float y = Height / 2f, left = 8, width = Width - 16, h = hover || Held ? 6 : 4;
            using (var track = new SolidBrush(Rule)) FillRound(g, track, left, y - h / 2, width, h);
            using (var done = new SolidBrush(Clay)) FillRound(g, done, left, y - h / 2, Math.Max(h, width * value), h);
            if (hover || Held) { using var thumb = new SolidBrush(Ink); g.FillEllipse(thumb, left + width * value - 7, y - 7, 14, 14); }
        }
        static void FillRound(Graphics g, Brush b, float x, float y, float w, float h)
        {
            using var p = new GraphicsPath(); var r = h;
            p.AddArc(x, y, r, r, 90, 180); p.AddArc(x + w - r, y, r, r, 270, 180); p.CloseFigure();
            g.FillPath(b, p);
        }
    }
}
