package com.hyll.wyble.wxapi;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;

/**
 * Прямоугольная пунктирная рамка с анимируемым смещением пунктира
 * (эффект "бегущей змейки" по периметру квадрата).
 * Не зависит от GradientDrawable.setStrokeDashOffset (доступного только с API 28+).
 */
public class ValetBorderDrawable extends Drawable {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float dash;
    private final float gap;
    private final float width;
    private final float[] intervals;

    public ValetBorderDrawable(int color, float width, float dash, float gap) {
        this.width = width;
        this.dash = dash;
        this.gap = gap;
        this.intervals = new float[]{dash, gap};
        paint.setColor(color);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(width);
        paint.setPathEffect(new DashPathEffect(intervals, 0f));
    }

    /** Установить смещение пунктира (анимируется снаружи). */
    public void setDashOffset(float offset) {
        paint.setPathEffect(new DashPathEffect(intervals, offset));
        invalidateSelf();
    }

    @Override
    public void draw(Canvas canvas) {
        Rect r = getBounds();
        float pad = width / 2f + 0.5f;
        canvas.drawRect(r.left + pad, r.top + pad, r.right - pad, r.bottom - pad, paint);
    }

    @Override
    public void setAlpha(int alpha) {
        paint.setAlpha(alpha);
    }

    @Override
    public void setColorFilter(ColorFilter colorFilter) {
        paint.setColorFilter(colorFilter);
    }

    @Override
    public int getOpacity() {
        return android.graphics.PixelFormat.TRANSLUCENT;
    }
}
