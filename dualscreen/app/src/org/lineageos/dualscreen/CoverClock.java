/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.dualscreen;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.format.DateFormat;

import java.util.Date;
import java.util.Locale;

final class CoverClock {
    private CoverClock() {}

    static byte[] render(Context context, int width, int height, int battery, boolean charging) {
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.BLACK);
        canvas.scale(width / 240f, height / 128f);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(Color.WHITE);
        paint.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        paint.setTextAlign(Paint.Align.CENTER);
        Date now = new Date();
        String time = DateFormat.getTimeFormat(context).format(now);
        paint.setTextSize(42);
        if (paint.measureText(time) > 220) {
            paint.setTextSize(42 * 220 / paint.measureText(time));
        }
        canvas.drawText(time, 120, 52, paint);
        paint.setTextSize(17);
        String pattern = DateFormat.getBestDateTimePattern(Locale.getDefault(), "EEE d MMM");
        canvas.drawText(new java.text.SimpleDateFormat(pattern, Locale.getDefault()).format(now),
                120, 83, paint);

        String percent = battery < 0 ? "--%" : battery + "%";
        paint.setTextSize(18);
        paint.setTextAlign(Paint.Align.LEFT);
        float start = (240 - 34 - paint.measureText(percent)) / 2;
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2);
        canvas.drawRect(start, 100, start + 24, 112, paint);
        canvas.drawLine(start + 26, 103, start + 26, 109, paint);
        paint.setStyle(Paint.Style.FILL);
        if (battery > 0) canvas.drawRect(start + 3, 103, start + 3 + 18 * battery / 100f, 109, paint);
        if (charging) {
            // A small charging mark above the battery keeps the percentage legible.
            canvas.drawLine(start + 9, 92, start + 15, 92, paint);
            canvas.drawLine(start + 12, 89, start + 12, 95, paint);
        }
        canvas.drawText(percent, start + 34, 113, paint);
        int[] colors = new int[width * height];
        bitmap.getPixels(colors, 0, width, 0, 0, width, height);
        bitmap.recycle();
        byte[] pixels = new byte[colors.length];
        for (int i = 0; i < colors.length; i++) pixels[i] = (byte) (colors[i] & 0xff);
        return pixels;
    }
}
