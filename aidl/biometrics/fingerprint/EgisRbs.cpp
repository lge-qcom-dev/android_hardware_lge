/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include <log/log.h>

#include "EgisRbs.h"

rbs_fingerprint_device_t* RBS_LoadLibrary(void) {
    int err;
    void* rbs_handle;
    uint8_t masterkey[0x100];

    ALOGD("Opening fingerprint hal library...");
    rbs_handle = dlopen("libRbsFlow.so", RTLD_NOW);

    if (rbs_handle == NULL) {
        ALOGE("No valid fingerprint module");
        return NULL;
    }

    rbs_fingerprint_device_t* fp_device =
            (rbs_fingerprint_device_t*)malloc(sizeof(rbs_fingerprint_device_t));

    fp_device->rbs_initialize = reinterpret_cast<typeof(fp_device->rbs_initialize)>(
            dlsym(rbs_handle, "rbs_initialize"));

    fp_device->rbs_uninitialize = reinterpret_cast<typeof(fp_device->rbs_uninitialize)>(
            dlsym(rbs_handle, "rbs_uninitialize"));

    fp_device->rbs_cancel =
            reinterpret_cast<typeof(fp_device->rbs_cancel)>(dlsym(rbs_handle, "rbs_cancel"));

    fp_device->rbs_active_user_group = reinterpret_cast<typeof(fp_device->rbs_active_user_group)>(
            dlsym(rbs_handle, "rbs_active_user_group"));

    fp_device->rbs_set_data_path = reinterpret_cast<typeof(fp_device->rbs_set_data_path)>(
            dlsym(rbs_handle, "rbs_set_data_path"));

    fp_device->rbs_chk_secure_id = reinterpret_cast<typeof(fp_device->rbs_chk_secure_id)>(
            dlsym(rbs_handle, "rbs_chk_secure_id"));

    fp_device->rbs_pre_enroll = reinterpret_cast<typeof(fp_device->rbs_pre_enroll)>(
            dlsym(rbs_handle, "rbs_pre_enroll"));

    fp_device->rbs_enroll =
            reinterpret_cast<typeof(fp_device->rbs_enroll)>(dlsym(rbs_handle, "rbs_enroll"));

    fp_device->rbs_post_enroll = reinterpret_cast<typeof(fp_device->rbs_post_enroll)>(
            dlsym(rbs_handle, "rbs_post_enroll"));

    fp_device->rbs_chk_auth_token = reinterpret_cast<typeof(fp_device->rbs_chk_auth_token)>(
            dlsym(rbs_handle, "rbs_chk_auth_token"));

    fp_device->rbs_authenticator = reinterpret_cast<typeof(fp_device->rbs_authenticator)>(
            dlsym(rbs_handle, "rbs_authenticator"));

    fp_device->rbs_remove_fingerprint = reinterpret_cast<typeof(fp_device->rbs_remove_fingerprint)>(
            dlsym(rbs_handle, "rbs_remove_fingerprint"));

    fp_device->rbs_get_fingerprint_ids =
            reinterpret_cast<typeof(fp_device->rbs_get_fingerprint_ids)>(
                    dlsym(rbs_handle, "rbs_get_fingerprint_ids"));

    fp_device->rbs_get_authenticator_id =
            reinterpret_cast<typeof(fp_device->rbs_get_authenticator_id)>(
                    dlsym(rbs_handle, "rbs_get_authenticator_id"));

    fp_device->rbs_set_on_callback_proc =
            reinterpret_cast<typeof(fp_device->rbs_set_on_callback_proc)>(
                    dlsym(rbs_handle, "rbs_set_on_callback_proc"));

    fp_device->rbs_extra_api =
            reinterpret_cast<typeof(fp_device->rbs_extra_api)>(dlsym(rbs_handle, "rbs_extra_api"));

    fp_device->rbs_get_challenge = reinterpret_cast<typeof(fp_device->rbs_get_challenge)>(
            dlsym(rbs_handle, "rbs_get_challenge"));

    fp_device->rbs_post_challenge = reinterpret_cast<typeof(fp_device->rbs_post_challenge)>(
            dlsym(rbs_handle, "rbs_post_challenge"));

    fp_device->g_custom_ini_path = reinterpret_cast<typeof(fp_device->g_custom_ini_path)>(
            dlsym(rbs_handle, "g_custom_ini_path"));

    RBS_GetSecureKey(masterkey, sizeof(masterkey));

    if ((err = fp_device->rbs_initialize(masterkey, sizeof(masterkey))) != 0) {
        ALOGE("Can't open fingerprint, error %d", err);
        free(fp_device);
        return NULL;
    }

    return fp_device;
}

#define ETS_KEYMASTER_CMD_GET_SECURE_KEY 0x200000205ull

int RBS_GetSecureKey(void* masterkey, uint32_t size) {
    int rc = 0;
    int (*qsc_start_app)(struct QSEECom_handle * *clnt_handle, const char* fname, uint32_t sb_size);
    int (*qsc_shutdown_app)(struct QSEECom_handle * *clnt_handle);
    int (*ets_keymaster_send_cmd)(struct QSEECom_handle * clnt_handle, void* send_buf,
                                  uint32_t sbuf_len, void* rcv_buf, uint32_t* rbuf_len);
    struct QSEECom_handle* mKeymasterHandle = NULL;
    uint64_t send_cmd = ETS_KEYMASTER_CMD_GET_SECURE_KEY;
    struct ets_masterkey_response rcv_buf;
    uint32_t rcv_buf_size = sizeof(rcv_buf);
    void* ets_teeclient_handle = NULL;

    ets_teeclient_handle = dlopen("libets_teeclient_v2.so", RTLD_NOW);
    if (ets_teeclient_handle == nullptr) {
        ALOGE("Cannot load TEE client");
        return false;
    }

    qsc_start_app =
            reinterpret_cast<typeof(qsc_start_app)>(dlsym(ets_teeclient_handle, "qsc_start_app"));

    qsc_shutdown_app = reinterpret_cast<typeof(qsc_shutdown_app)>(
            dlsym(ets_teeclient_handle, "qsc_shutdown_app"));

    ets_keymaster_send_cmd = reinterpret_cast<typeof(ets_keymaster_send_cmd)>(
            dlsym(ets_teeclient_handle, "ets_keymaster_issue_send_modified_cmd_req"));

    rc = qsc_start_app(&mKeymasterHandle, "keymaster64", 0x2400);
    if (rc) {
        ALOGE("Cannot load keymaster application, error %d", rc);
        return rc;
    }

    rc = ets_keymaster_send_cmd(mKeymasterHandle, &send_cmd, 8, &rcv_buf, &rcv_buf_size);
    if (rc) {
        ALOGE("Cannot send keymaster cmd, error %d", rc);
        goto shutdown;
    }

    if (rcv_buf.rc != 0) {
        ALOGE("Get master key failed, error %d", rcv_buf.rc);
        rc = rcv_buf.rc;
        goto shutdown;
    }

    if (size < rcv_buf.size) {
        ALOGE("Output buffer too short, expected size %d, got size %d", rcv_buf.size, size);
        rc = -ENOMEM;
        goto shutdown;
    }

    memcpy(masterkey, rcv_buf.masterkey, size);
shutdown:
    qsc_shutdown_app(&mKeymasterHandle);
    dlclose(ets_teeclient_handle);

    qsc_start_app = NULL;
    qsc_shutdown_app = NULL;
    ets_keymaster_send_cmd = NULL;
    return rc;
}
