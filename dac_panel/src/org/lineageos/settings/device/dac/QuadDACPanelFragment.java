package org.lineageos.settings.device.dac;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioManager;
import android.os.Bundle;
import android.provider.Settings;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceFragment;
import androidx.preference.Preference.OnPreferenceChangeListener;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.SwitchPreference;
import androidx.preference.ListPreference;
import androidx.preference.SeekBarPreference;

import android.util.Log;
import android.view.MenuItem;

import org.lineageos.settings.device.dac.ui.BalancePreference;
import org.lineageos.settings.device.dac.ui.ButtonPreference;
import org.lineageos.settings.device.dac.utils.Constants;
import org.lineageos.settings.device.dac.utils.QuadDAC;

import java.util.ArrayList;

import vendor.lge.hardware.audio.dac.control.V2_0.Feature;
import vendor.lge.hardware.audio.dac.control.V2_0.IDacControl;
import vendor.lge.hardware.audio.dac.control.V2_0.Range;

public class QuadDACPanelFragment extends PreferenceFragment
        implements OnPreferenceChangeListener {

    private static final String TAG = "QuadDACPanelFragment";

    private QuadDAC DacControlInterface = new QuadDAC();

    private SwitchPreference quaddac_switch;
    private ListPreference sound_preset_list, digital_filter_list, mode_list;
    private BalancePreference balance_preference;
    private SeekBarPreference avc_volume;

    /*** Custom filter UI props ***/

    /* Shape and symmetry selectors */
    private ListPreference custom_filter_shape, custom_filter_symmetry;

    /* Filter stage 2 coefficients (refer to the kernel's es9218.c for more info) */
    private static SeekBarPreference[] custom_filter_coeffs = new SeekBarPreference[14];

    /* Button to reset custom filter's coefficients, if needed. */
    private ButtonPreference custom_filter_reset_coeffs_button;

    private HeadsetPluggedFragmentReceiver headsetPluggedFragmentReceiver;

    private IDacControl dac;

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        headsetPluggedFragmentReceiver = new HeadsetPluggedFragmentReceiver();
        try {
            DacControlInterface.initialize();
        } catch(Exception e) {
            Log.d(TAG, "onCreatePreferences: " + e.toString());
        }
        addPreferencesFromResource(R.xml.quaddac_panel);
    }

    public static void setCoeffSummary(int index, int value) {
        custom_filter_coeffs[index].setValue(value);
        custom_filter_coeffs[index].setSummary("Coefficient " + index + " : 0." + value);
    }

    @Override
    public boolean onPreferenceChange(Preference preference, Object newValue) {

        try {
            if (preference instanceof SwitchPreference) {

                boolean set_dac_on = (boolean) newValue;

                if (set_dac_on) {
                    DacControlInterface.enable();
                    enableExtraSettings();
                    return true;
                } else {
                    DacControlInterface.disable();
                    disableExtraSettings();
                    return true;
                }
            }
            if (preference instanceof ListPreference) {
                if (preference.getKey().equals(Constants.HIFI_MODE_KEY)) {
                    ListPreference lp = (ListPreference) preference;

                    int mode = lp.findIndexOfValue((String) newValue);
                    DacControlInterface.setDACMode(mode);
                    return true;

                } else if (preference.getKey().equals(Constants.DIGITAL_FILTER_KEY)) {
                    ListPreference lp = (ListPreference) preference;

                    int digital_filter = lp.findIndexOfValue((String) newValue);
                    DacControlInterface.setDigitalFilter(digital_filter);

                    /* Custom filter panel should only show up with Filter [3] (fourth one) selected */
                    if(DacControlInterface.getSupportedFeatures().contains(Feature.CustomFilter) && digital_filter == 3)
                        enableCustomFilter();
                    else
                        disableCustomFilter();

                    return true;

                } else if (preference.getKey().equals(Constants.SOUND_PRESET_KEY)) {
                    ListPreference lp = (ListPreference) preference;

                    int sound_preset = lp.findIndexOfValue((String) newValue);
                    DacControlInterface.setSoundPreset(sound_preset);
                    return true;
                } else if(preference.getKey().equals(Constants.CUSTOM_FILTER_SHAPE_KEY))
                {
                    ListPreference lp = (ListPreference) preference;

                    int filter_shape = lp.findIndexOfValue((String) newValue);
                    DacControlInterface.setCustomFilterShape(filter_shape);
                    return true;

                } else if(preference.getKey().equals(Constants.CUSTOM_FILTER_SYMMETRY_KEY))
                {
                    ListPreference lp = (ListPreference) preference;

                    int filter_symmetry = lp.findIndexOfValue((String) newValue);
                    DacControlInterface.setCustomFilterSymmetry(filter_symmetry);
                    return true;

                }
                return false;
            }

            if (preference instanceof SeekBarPreference) {
                if (preference.getKey().equals(Constants.AVC_VOLUME_KEY)) {
                    if (newValue instanceof Integer) {
                        Integer avc_vol = (Integer) newValue;

                        //avc_volume.setSummary( ((double)avc_vol) + " db");

                        DacControlInterface.setAVCVolume(avc_vol);
                        return true;
                    } else {
                        return false;
                    }
                }
                else { /* This assumes the only other seekbars are for the custom filter. Extend as needed. */
                    for(int i = 0; i < 14; i++) {
                        if(preference.getKey().equals(Constants.CUSTOM_FILTER_COEFF_KEYS[i]))
                        {
                            if (newValue instanceof Integer) {
                                Integer coeffVal = (Integer) newValue;

                                setCoeffSummary(i, coeffVal);

                                DacControlInterface.setCustomFilterCoeff(i, coeffVal);
                                return true;
                            } else
                                return false;
                        }
                    }
                }
            }
        } catch (Exception e) {
            Log.d(TAG, "onPreferenceChange: " + e.toString());
        }
        return false;
    }

    @Override
    public void onResume() {
        IntentFilter filter = new IntentFilter(Intent.ACTION_HEADSET_PLUG);
        getActivity().registerReceiver(headsetPluggedFragmentReceiver, filter);
        super.onResume();
    }

    @Override
    public void onPause() {
        getActivity().unregisterReceiver(headsetPluggedFragmentReceiver);
        super.onPause();
    }

    @Override
    public void addPreferencesFromResource(int preferencesResId) {
        super.addPreferencesFromResource(preferencesResId);
        // Initialize preferences
        AudioManager am = getContext().getSystemService(AudioManager.class);

        quaddac_switch = (SwitchPreference) findPreference(Constants.DAC_SWITCH_KEY);
        quaddac_switch.setOnPreferenceChangeListener(this);

        sound_preset_list = (ListPreference) findPreference(Constants.SOUND_PRESET_KEY);
        sound_preset_list.setOnPreferenceChangeListener(this);

        digital_filter_list = (ListPreference) findPreference(Constants.DIGITAL_FILTER_KEY);
        digital_filter_list.setOnPreferenceChangeListener(this);

        custom_filter_shape = (ListPreference) findPreference(Constants.CUSTOM_FILTER_SHAPE_KEY);
        custom_filter_shape.setOnPreferenceChangeListener(this);

        custom_filter_symmetry = (ListPreference) findPreference(Constants.CUSTOM_FILTER_SYMMETRY_KEY);
        custom_filter_symmetry.setOnPreferenceChangeListener(this);

        custom_filter_reset_coeffs_button = (ButtonPreference) findPreference(Constants.RESET_COEFFICIENTS_KEY);
        custom_filter_reset_coeffs_button.setOnPreferenceChangeListener(this);

        for(int i = 0; i < 14; i++)
        {
            custom_filter_coeffs[i] = (SeekBarPreference) findPreference(Constants.CUSTOM_FILTER_COEFF_KEYS[i]);
            custom_filter_coeffs[i].setOnPreferenceChangeListener(this);
        }

        mode_list = (ListPreference) findPreference(Constants.HIFI_MODE_KEY);
        mode_list.setOnPreferenceChangeListener(this);

        avc_volume = (SeekBarPreference) findPreference(Constants.AVC_VOLUME_KEY);
        avc_volume.setOnPreferenceChangeListener(this);

        balance_preference = (BalancePreference) findPreference(Constants.BALANCE_KEY);

        try {
            if (DacControlInterface.getSupportedFeatures().contains(Feature.QuadDAC)) {
                quaddac_switch.setVisible(true);
            }
            if (DacControlInterface.getSupportedFeatures().contains(Feature.SoundPreset)) {
                sound_preset_list.setVisible(true);
                sound_preset_list.setValueIndex(DacControlInterface.getSoundPreset());
            }
            if (DacControlInterface.getSupportedFeatures().contains(Feature.DigitalFilter)) {
                digital_filter_list.setVisible(true);
                digital_filter_list.setValueIndex(DacControlInterface.getDigitalFilter());
            }
            if (DacControlInterface.getSupportedFeatures().contains(Feature.BalanceLeft)
                    && DacControlInterface.getSupportedFeatures().contains(Feature.BalanceRight)) {
                balance_preference.setVisible(true);
            }
            if (DacControlInterface.getSupportedFeatures().contains(Feature.AVCVolume)) {
                avc_volume.setVisible(true);
                Range range = DacControlInterface.getSupportedFeatureValues(Feature.AVCVolume).range;
                avc_volume.setMin((int)range.min);
                avc_volume.setMax((int)range.max);
                avc_volume.setValue(DacControlInterface.getAVCVolume());
            }
            if (DacControlInterface.getSupportedFeatures().contains(Feature.HifiMode)) {
                mode_list.setVisible(true);
                mode_list.setValueIndex(DacControlInterface.getDACMode());
            }
            if (DacControlInterface.getSupportedFeatures().contains(Feature.CustomFilter)) {
                custom_filter_shape.setVisible(true);
                custom_filter_shape.setValueIndex(DacControlInterface.getCustomFilterShape());
                custom_filter_symmetry.setVisible(true);
                custom_filter_symmetry.setValueIndex(DacControlInterface.getCustomFilterSymmetry());
                for(int i = 0; i < 14; i++)
                {
                    custom_filter_coeffs[i].setVisible(true);
                    custom_filter_coeffs[i].setValue(DacControlInterface.getCustomFilterCoeff(i));
                    setCoeffSummary(i, DacControlInterface.getCustomFilterCoeff(i));
                }
            }
        } catch(Exception e) {
            Log.d(TAG, "addPreferencesFromResource: " + e.toString());
        }

        try {
            if (am.isWiredHeadsetOn()) {
                quaddac_switch.setEnabled(true);
                if (DacControlInterface.isEnabled()) {
                    quaddac_switch.setChecked(true);
                    enableExtraSettings();
                } else {
                    quaddac_switch.setChecked(false);
                    disableExtraSettings();
                }
            } else {
                quaddac_switch.setEnabled(false);
                disableExtraSettings();
                if (DacControlInterface.isEnabled()) {
                    quaddac_switch.setChecked(true);
                }
            }
        } catch (Exception e) {
            Log.d(TAG, "addPreferencesFromResource2: " + e.toString());
        }
    }

    private void enableExtraSettings()
    {
        ArrayList<Integer> supportedFeatures = DacControlInterface.getSupportedFeatures();
        digital_filter_list.setEnabled(true);
        mode_list.setEnabled(true);
        avc_volume.setEnabled(true);
        balance_preference.setEnabled(true);
        if(supportedFeatures.contains(Feature.SoundPreset))
            sound_preset_list.setEnabled(true);
        if(supportedFeatures.contains(Feature.CustomFilter))
            enableCustomFilter();
    }

    private void disableExtraSettings()
    {
        digital_filter_list.setEnabled(false);
        mode_list.setEnabled(false);
        avc_volume.setEnabled(false);
        balance_preference.setEnabled(false);
        sound_preset_list.setEnabled(false);
        disableCustomFilter();
    }

    private void enableCustomFilter()
    {
        checkCustomFilterVisibility();
        custom_filter_shape.setEnabled(true);
        custom_filter_symmetry.setEnabled(true);
        for(int i = 0; i < 14; i++){
            custom_filter_coeffs[i].setEnabled(true);
        }

        custom_filter_reset_coeffs_button.setEnabled(true);

        try {
            /* To apply the custom filter's settings */
            DacControlInterface.setCustomFilterShape(DacControlInterface.getCustomFilterShape());
        } catch (Exception e) {}
    }

    private void disableCustomFilter()
    {
        checkCustomFilterVisibility();
        custom_filter_shape.setEnabled(false);
        custom_filter_symmetry.setEnabled(false);
        for(int i = 0; i < 14; i++)
            custom_filter_coeffs[i].setEnabled(false);

        custom_filter_reset_coeffs_button.setEnabled(false);
    }

    private void checkCustomFilterVisibility() {
        /* 
         * If the selected digital filter is the custom filter,
         * its preferences should be visible. Otherwise, hide them
         * to remove unused preferences from the panel.
        */
        try {
            if(DacControlInterface.getDigitalFilter() == 3) {
                custom_filter_shape.setVisible(true);
                custom_filter_symmetry.setVisible(true);
                for(int i = 0; i < 14; i++)
                    custom_filter_coeffs[i].setVisible(true);
            }
            else {
                custom_filter_shape.setVisible(false);
                custom_filter_symmetry.setVisible(false);
                for(int i = 0; i < 14; i++)
                    custom_filter_coeffs[i].setVisible(false);
            }
        } catch (Exception e) {}
    }

    private class HeadsetPluggedFragmentReceiver extends BroadcastReceiver {

        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent.getAction().equals(Intent.ACTION_HEADSET_PLUG)) {
                int state = intent.getIntExtra("state", -1);
                switch(state)
                {
                    case 1: // Headset plugged in
                        quaddac_switch.setEnabled(true);
                        if(quaddac_switch.isChecked())
                            enableExtraSettings();
                        break;
                    case 0: // Headset unplugged
                        quaddac_switch.setEnabled(false);
                        disableExtraSettings();
                        break;
                    default: break;
                }
            }
        }
    }

    public IDacControl getDhc() { return dac; }

}
