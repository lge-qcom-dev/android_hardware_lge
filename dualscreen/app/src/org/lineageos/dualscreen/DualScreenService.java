/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.dualscreen;

import android.app.AlarmManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.hardware.display.BrightnessInfo;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import vendor.lge.hardware.dualscreen.CaseInfo;
import vendor.lge.hardware.dualscreen.ICaseCallback;
import vendor.lge.hardware.dualscreen.IDualScreen;

public final class DualScreenService extends Service implements DisplayManager.DisplayListener,
        InputManager.InputDeviceListener {
    private static final String TAG = "LgeDualScreen";
    private static final String HAL = IDualScreen.DESCRIPTOR + "/default";
    private final HandlerThread mThread = new HandlerThread(TAG);
    private Handler mHandler;
    private DisplayManager mDisplays;
    private InputManager mInputs;
    private PowerManager mPower;
    private AlarmManager mAlarms;
    private IDualScreen mHal;
    private CaseInfo mInfo = new CaseInfo();
    private int mBattery = -1;
    private boolean mCharging;
    private String mDisplayUniqueId;
    private int mConfiguredDisplay = Display.INVALID_DISPLAY;
    private final Map<String, String> mAssociations = new HashMap<>();
    private final Runnable mConnect = this::connect;
    private final AlarmManager.OnAlarmListener mMinute = this::refresh;
    private final IBinder.DeathRecipient mDeath = () -> mHandler.post(this::halDied);
    private final ICaseCallback mCallback = new ICaseCallback.Stub() {
        @Override
        public void onCaseChanged(CaseInfo info) {
            mHandler.post(() -> caseChanged(info));
        }

        @Override
        public int getInterfaceVersion() {
            return ICaseCallback.VERSION;
        }

        @Override
        public String getInterfaceHash() {
            return ICaseCallback.HASH;
        }
    };
    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Intent.ACTION_BATTERY_CHANGED.equals(intent.getAction())) {
                int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                mBattery = level >= 0 && scale > 0 ? Math.min(100, level * 100 / scale) : -1;
                mCharging = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
            }
            refresh();
        }
    };
    private ContentObserver mSettings;

    @Override
    public void onCreate() {
        super.onCreate();
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        mDisplays = getSystemService(DisplayManager.class);
        mInputs = getSystemService(InputManager.class);
        mPower = getSystemService(PowerManager.class);
        mAlarms = getSystemService(AlarmManager.class);
        mDisplays.registerDisplayListener(this, mHandler,
                DisplayManager.EVENT_TYPE_DISPLAY_ADDED | DisplayManager.EVENT_TYPE_DISPLAY_REMOVED
                | DisplayManager.EVENT_TYPE_DISPLAY_CHANGED | DisplayManager.EVENT_TYPE_DISPLAY_BRIGHTNESS);
        mInputs.registerInputDeviceListener(this, mHandler);
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_SCREEN_OFF);
        filter.addAction(Intent.ACTION_BATTERY_CHANGED);
        filter.addAction(Intent.ACTION_TIME_CHANGED);
        filter.addAction(Intent.ACTION_TIMEZONE_CHANGED);
        filter.addAction(Intent.ACTION_LOCALE_CHANGED);
        registerReceiver(mReceiver, filter, null, mHandler, Context.RECEIVER_NOT_EXPORTED);
        mSettings = new ContentObserver(mHandler) {
            @Override
            public void onChange(boolean selfChange) { refresh(); }
        };
        getContentResolver().registerContentObserver(
                Settings.System.getUriFor(Settings.System.TIME_12_24), false, mSettings);
        mHandler.post(mConnect);
    }

    private void connect() {
        if (mHal != null) return;
        IBinder binder = ServiceManager.getService(HAL);
        if (binder == null) {
            mHandler.postDelayed(mConnect, 5000);
            return;
        }
        try {
            binder.linkToDeath(mDeath, 0);
            mHal = IDualScreen.Stub.asInterface(binder);
            mHal.setCallback(mCallback);
            refresh();
        } catch (RemoteException e) {
            halDied();
        }
    }

    private void halDied() {
        if (mHal != null) mHal.asBinder().unlinkToDeath(mDeath, 0);
        mHal = null;
        mInfo = new CaseInfo();
        mDisplayUniqueId = null;
        associateTouch();
        mAlarms.cancel(mMinute);
        mHandler.removeCallbacks(mConnect);
        mHandler.postDelayed(mConnect, 1000);
    }

    private void caseChanged(CaseInfo info) {
        CaseInfo previous = mInfo;
        mInfo = info;
        if (!info.connected) {
            mDisplayUniqueId = null;
            mConfiguredDisplay = Display.INVALID_DISPLAY;
        }
        if (info.connected && info.coverState == 1
                && (!previous.connected || previous.coverState != 1)) {
            mPower.goToSleep(SystemClock.uptimeMillis());
        } else if (info.connected && previous.connected && previous.coverState == 1
                && info.coverState == 0) {
            mPower.wakeUp(SystemClock.uptimeMillis(), PowerManager.WAKE_REASON_WAKE_MOTION,
                    "dualscreen:open");
        }
        associateTouch();
        refresh();
    }

    private void associateTouch() {
        if (mInfo.connected) {
            Display external = null;
            int count = 0;
            for (Display display : mDisplays.getDisplays(
                    DisplayManager.DISPLAY_CATEGORY_ALL_INCLUDING_DISABLED)) {
                if (display.getType() == Display.TYPE_EXTERNAL) {
                    external = display;
                    count++;
                }
            }
            // Refuse to guess when multiple external displays are present.
            if (count == 1) {
                mDisplayUniqueId = external.getUniqueId();
                if (mConfiguredDisplay != external.getDisplayId()) {
                    try {
                        mDisplays.enableConnectedDisplay(external.getDisplayId());
                        WindowManagerGlobal.getWindowManagerService().setDisplayImePolicy(
                                external.getDisplayId(), WindowManager.DISPLAY_IME_POLICY_LOCAL);
                        mConfiguredDisplay = external.getDisplayId();
                    } catch (RemoteException e) {
                        Log.w(TAG, "Cannot configure case display", e);
                    }
                }
            }
        }
        Set<String> desired = new HashSet<>();
        if (mInfo.connected && mDisplayUniqueId != null) {
            for (int id : mInputs.getInputDeviceIds()) {
                InputDevice device = mInputs.getInputDevice(id);
                if (device != null && device.getVendorId() == 0x1004
                        && device.getProductId() == 0x637a
                        && device.supportsSource(InputDevice.SOURCE_TOUCHSCREEN)) {
                    String descriptor = device.getDescriptor();
                    desired.add(descriptor);
                    if (!mDisplayUniqueId.equals(mAssociations.get(descriptor))) {
                        mInputs.addUniqueIdAssociationByDescriptor(descriptor, mDisplayUniqueId);
                        mAssociations.put(descriptor, mDisplayUniqueId);
                    }
                }
            }
        }
        for (String descriptor : new HashSet<>(mAssociations.keySet())) {
            if (!desired.contains(descriptor)) {
                mInputs.removeUniqueIdAssociationByDescriptor(descriptor);
                mAssociations.remove(descriptor);
            }
        }
    }

    private void refresh() {
        mAlarms.cancel(mMinute);
        if (mHal == null) return;
        Display primary = mDisplays.getDisplay(Display.DEFAULT_DISPLAY);
        BrightnessInfo brightness = primary == null ? null : primary.getBrightnessInfo();
        float value = brightness == null || !Float.isFinite(brightness.brightness)
                ? 0.5f : brightness.brightness;
        int level = Math.round(Math.max(0, Math.min(1, value)) * 255);
        boolean interactive = mPower.isInteractive(Display.DEFAULT_DISPLAY);
        Display external = mDisplays.getDisplay(mConfiguredDisplay);
        if (mInfo.connected && external != null) {
            boolean awake = interactive && mInfo.coverState == 0;
            if (awake != mPower.isInteractive(external.getDisplayId())) {
                long now = SystemClock.uptimeMillis();
                if (awake) {
                    mPower.wakeUp(now, PowerManager.WAKE_REASON_APPLICATION,
                            "dualscreen:display", external.getDisplayId());
                } else {
                    mPower.goToSleep(external.getDisplayId(), now,
                            PowerManager.GO_TO_SLEEP_REASON_APPLICATION, 0);
                }
            }
        }
        try {
            mHal.setDisplayState(interactive, level);
            if (mInfo.connected && mInfo.coverState == 1 && mInfo.width > 0 && mInfo.height > 0) {
                mHal.drawCover(mInfo.width, mInfo.height, CoverClock.render(this, mInfo.width,
                        mInfo.height, mBattery, mCharging));
                long nextMinute = (System.currentTimeMillis() / 60000 + 1) * 60000;
                mAlarms.setExact(AlarmManager.RTC_WAKEUP, nextMinute, TAG, mMinute, mHandler);
            }
        } catch (RemoteException e) {
            halDied();
        } catch (IllegalArgumentException e) {
            // A detach or panel change can race an already queued framework update.
            Log.w(TAG, "Cover display changed during update", e);
        }
    }

    @Override public void onDisplayAdded(int id) { associateTouch(); refresh(); }
    @Override public void onDisplayRemoved(int id) {
        if (mConfiguredDisplay == id) mConfiguredDisplay = Display.INVALID_DISPLAY;
        associateTouch();
        refresh();
    }
    @Override public void onDisplayChanged(int id) { associateTouch(); refresh(); }
    @Override public void onInputDeviceAdded(int id) { associateTouch(); }
    @Override public void onInputDeviceRemoved(int id) { associateTouch(); }
    @Override public void onInputDeviceChanged(int id) { associateTouch(); }
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public int onStartCommand(Intent intent, int flags, int id) { return START_STICKY; }

    @Override
    public void onDestroy() {
        unregisterReceiver(mReceiver);
        getContentResolver().unregisterContentObserver(mSettings);
        mDisplays.unregisterDisplayListener(this);
        mInputs.unregisterInputDeviceListener(this);
        mHandler.post(() -> {
            mAlarms.cancel(mMinute);
            if (mHal != null) {
                try { mHal.setCallback(null); } catch (RemoteException ignored) {}
                mHal.asBinder().unlinkToDeath(mDeath, 0);
                mHal = null;
            }
            for (String descriptor : new HashSet<>(mAssociations.keySet())) {
                mInputs.removeUniqueIdAssociationByDescriptor(descriptor);
            }
            mThread.quitSafely();
        });
        super.onDestroy();
    }
}
