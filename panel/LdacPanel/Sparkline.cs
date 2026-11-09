using System;
using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace LdacPanel;

/// <summary>最近若干包的 peak 曲线，自己画，不引图表库。</summary>
public sealed class Sparkline : Control
{
    private const int Capacity = 260;
    private readonly List<double> _values = new();
    private static readonly IBrush Line = SolidColorBrush.Parse("#5AC8FA");
    private static readonly IBrush Fill = SolidColorBrush.Parse("#1E3A4A");

    public void Push(double value)
    {
        _values.Add(value);
        if (_values.Count > Capacity) _values.RemoveAt(0);
        InvalidateVisual();
    }

    public void Clear()
    {
        _values.Clear();
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        var width = Bounds.Width;
        var height = Bounds.Height;
        if (width <= 1 || height <= 1) return;

        if (_values.Count < 2)
        {
            context.DrawLine(new Pen(Brushes.DimGray, 1), new Point(0, height - 1), new Point(width, height - 1));
            return;
        }

        var max = 1.0;
        foreach (var v in _values) if (v > max) max = v;

        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            ctx.BeginFigure(new Point(0, height), true);
            for (var i = 0; i < _values.Count; i++)
            {
                var x = width * i / (_values.Count - 1);
                var y = height - _values[i] / max * (height - 3) - 1;
                ctx.LineTo(new Point(x, y));
            }
            ctx.LineTo(new Point(width, height));
            ctx.EndFigure(true);
        }

        context.DrawGeometry(Fill, new Pen(Line, 1.5), geo);
    }
}
