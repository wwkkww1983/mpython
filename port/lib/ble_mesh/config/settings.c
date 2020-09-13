/*
 * Copyright (c) 2018 Intel Corporation
 * Additional Copyright (c) 2018 Espressif Systems (Shanghai) PTE LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#define BT_DBG_ENABLED (0) //IS_ENABLED(CONFIG_BLE_MESH_DEBUG_SETTINGS)

#include "mesh/mesh.h"
#include "crypto.h"
// #include "transport.h"
// #include "access.h"
// #include "foundation.h"
// #include "proxy_server.h"
#include "mesh/cfg_srv.h"
#include "net.h"
#include "crypto.h"
#include "transport.h"
#include "access.h"
#include "foundation.h"
#include "proxy.h"
#include "settings.h"
#include "config/settings_nvs.h"

/* BLE Mesh NVS Key and corresponding data struct.
 * Note: The length of nvs key must be <= 15.
 *       "xxxx" (2 octet) means the rpl_src, net_idx, app_idx, model_key, etc.
 *       Model model_key is a combination "elem_idx << 8 | mod_idx".
 * key: "mesh/net"    -> write/read to set/get NET data
 * key: "mesh/iv"     -> write/read to set/get IV data
 * key: "mesh/seq"    -> write/read to set/get SEQ data
 * key: "mesh/hb_pub" -> write/read to set/get CFG HB_PUB data
 * key: "mesh/cfg"    -> write/read to set/get CFG data
 * key: "mesh/rpl"    -> write/read to set/get all RPL src.
 *      key: "mesh/rpl/xxxx" -> write/read to set/get the "xxxx" RPL data
 * key: "mesh/netkey" -> write/read to set/get all NetKey Indexes
 *      key: "mesh/nk/xxxx" -> write/read to set/get the "xxxx" NetKey data
 * key: "mesh/appkey" -> write/read to set/get all AppKey Indexes
 *      key: "mesh/ak/xxxx" -> write/read to set/get the "xxxx" AppKey data
 * key: "mesh/sig" -> write/read to set/get all SIG MODEL model_keys.
 *      key: "mesh/s/xxxx/b"  -> write/read to set/get SIG MODEL Bind AppKey List
 *      key: "mesh/s/xxxx/s" -> write/read to set/get SIG MODEL Subscription List
 *      key: "mesh/s/xxxx/p" -> write/read to set/get SIG MODEL Publication
 * key: "mesh/vnd" -> write/read to set/get all VENDOR MODEL model_keys.
 *      key: "mesh/v/xxxx/b"  -> write/read to set/get VENDOR MODEL Bind AppKey List
 *      key: "mesh/v/xxxx/s" -> write/read to set/get VENDOR MODEL Subscription List
 *      key: "mesh/v/xxxx/p" -> write/read to set/get VENDOR MODEL Publication
 * key: "mesh/vaddr" -> write/read to set/get all virtual addresses
 *      key: "mesh/va/xxxx" -> write/read to set/get the "xxxx" virtual address
 */

// #if CONFIG_BLE_MESH_SETTINGS

/* BD ADDR length */
#define BLE_MESH_ADDR_LEN    0x06
#define CONFIG_BLE_MESH_NODE (1)

/* Unaligned access */
#define UNALIGNED_GET(p)                    \
__extension__ ({                            \
    struct  __attribute__((__packed__)) {   \
        __typeof__(*(p)) __v;               \
    } *__p = (__typeof__(__p)) (p);         \
    __p->__v;                               \
})

static inline int bt_mesh_atomic_get(const int *target)
{
    return __atomic_load_n(target, __ATOMIC_SEQ_CST);
}

static uint16_t simple_pull_le16(struct net_buf_simple *buf)
{
    uint16_t val = 0U;

    val = UNALIGNED_GET((uint16_t *)buf->data);
    buf->len -= sizeof(val);
    buf->data += sizeof(val);

    return sys_le16_to_cpu(val);
}

/* Tracking of what storage changes are pending for App and Net Keys. We
 * track this in a separate array here instead of within the respective
 * bt_mesh_app_key and bt_mesh_subnet structs themselves, since once a key
 * gets deleted its struct becomes invalid and may be reused for other keys.
 */
static struct key_update {
    u16_t key_idx: 12,   /* AppKey or NetKey Index */
          valid: 1,      /* 1 if this entry is valid, 0 if not */
          app_key: 1,    /* 1 if this is an AppKey, 0 if a NetKey */
          clear: 1;      /* 1 if key needs clearing, 0 if storing */
} key_updates[CONFIG_BT_MESH_APP_KEY_COUNT + CONFIG_BT_MESH_SUBNET_COUNT];

static struct k_delayed_work pending_store;

/* Mesh network storage information */
struct net_val {
    u16_t primary_addr;
    u8_t  dev_key[16];
} __packed;

/* Sequence number storage */
struct seq_val {
    u8_t val[3];
} __packed;

/* Heartbeat Publication storage */
struct hb_pub_val {
    u16_t dst;
    u8_t  period;
    u8_t  ttl;
    u16_t feat;
    u16_t net_idx: 12,
          indefinite: 1;
};

/* Miscelaneous configuration server model states */
struct cfg_val {
    u8_t net_transmit;
    u8_t relay;
    u8_t relay_retransmit;
    u8_t beacon;
    u8_t gatt_proxy;
    u8_t frnd;
    u8_t default_ttl;
};

/* IV Index & IV Update storage */
struct iv_val {
    u32_t iv_index;
    u8_t  iv_update: 1,
          iv_duration: 7;
} __packed;

/* Replay Protection List storage */
struct rpl_val {
    u32_t seq: 24,
          old_iv: 1;
};

/* NetKey storage information */
struct net_key_val {
    u8_t  kr_flag: 1,
          kr_phase: 7;
    u8_t  val[2][16];
} __packed;

/* AppKey storage information */
struct app_key_val {
    u16_t net_idx;
    bool  updated;
    u8_t  val[2][16];
} __packed;

struct mod_pub_val {
    u16_t addr;
    u16_t key;
    u8_t  ttl;
    u8_t  retransmit;
    u8_t  period;
    u8_t  period_div: 4,
          cred: 1;
};

/* Virtual Address information */
struct va_val {
    u16_t ref;
    u16_t addr;
    u8_t  uuid[16];
} __packed;

/* We need this so we don't overwrite app-hardcoded values in case FCB
 * contains a history of changes but then has a NULL at the end.
 */
static struct {
    bool valid;
    struct cfg_val cfg;
} stored_cfg;

struct prov_info {
    u16_t primary_addr;
    u16_t alloc_addr;
};

struct node_info {
    u8_t  addr[6];
    u8_t  addr_type;
    u8_t  dev_uuid[16];
    u16_t oob_info;
    u16_t unicast_addr;
    u8_t  element_num;
    u16_t net_idx;
    u8_t  flags;
    u32_t iv_index;
    u8_t  dev_key[16];
} __packed;

/* 从flash中还原bt_mesh中的dev_key并配置所有element地址*/
static int net_set(const char *name)
{
    struct net_val net = {0};
    bool exist = false;
    int err = 0;

    BT_DBG("%s", __func__);

    err = bt_mesh_load_core_settings(name, (u8_t *)&net, sizeof(net), &exist);
    if (err) {
        BT_WARN("%s, Clear NET", __func__);
        memset(bt_mesh.dev_key, 0, sizeof(bt_mesh.dev_key));
        bt_mesh_comp_unprovision();
        return 0;
    }

    if (exist == false) {
        return 0;
    }

    memcpy(bt_mesh.dev_key, net.dev_key, sizeof(bt_mesh.dev_key));
    bt_mesh_comp_provision(net.primary_addr);

    BT_DBG("Restored primary address 0x%04x", net.primary_addr);
    BT_DBG("Restored DevKey %s", bt_hex(bt_mesh.dev_key, 16));

    return 0;
}

/* 从flash中还原bt_mesh中的iv_index iv_update ivu_duration*/
static int iv_set(const char *name)
{
    struct iv_val iv = {0};
    bool exist = false;
    int err = 0;

    BT_DBG("%s", __func__);

    err = bt_mesh_load_core_settings(name, (u8_t *)&iv, sizeof(iv), &exist);
    if (err) {
        BT_WARN("%s, Clear IV", __func__);
        bt_mesh.iv_index = 0U;
        atomic_clear_bit(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS);
        return 0;
    }

    if (exist == false) {
        return 0;
    }

    bt_mesh.iv_index = iv.iv_index;
    atomic_set_bit_to(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS, iv.iv_update);
    bt_mesh.ivu_duration = iv.iv_duration;

    BT_DBG("Restored IV Index 0x%04x (IV Update Flag %u) duration %u hours",
           iv.iv_index, iv.iv_update, iv.iv_duration);

    return 0;
}

/* 从flash中还原bt_mesh中的seq*/
static int seq_set(const char *name)
{
    struct seq_val seq = {0};
    bool exist = false;
    int err = 0;

    BT_DBG("%s", __func__);

    err = bt_mesh_load_core_settings(name, (u8_t *)&seq, sizeof(seq), &exist);
    if (err) {
        BT_WARN("%s, Clear SEQ", __func__);
        bt_mesh.seq = 0U;
        return 0;
    }

    if (exist == false) {
        return 0;
    }

    bt_mesh.seq = ((u32_t)seq.val[0] | ((u32_t)seq.val[1] << 8) |
                   ((u32_t)seq.val[2] << 16));

#if CONFIG_BT_MESH_SEQ_STORE_RATE > 0
    /* Make sure we have a large enough sequence number. We
     * subtract 1 so that the first transmission causes a write
     * to the settings storage.
     */
    bt_mesh.seq += (CONFIG_BT_MESH_SEQ_STORE_RATE -
                    (bt_mesh.seq % CONFIG_BT_MESH_SEQ_STORE_RATE));
    bt_mesh.seq--;
#endif

    BT_DBG("Restored Sequence Number 0x%06x", bt_mesh.seq);

    return 0;
}

static struct bt_mesh_rpl *rpl_find(u16_t src)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        if (bt_mesh.rpl[i].src == src) {
            return &bt_mesh.rpl[i];
        }
    }

    return NULL;
}

static struct bt_mesh_rpl *rpl_alloc(u16_t src)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        if (bt_mesh.rpl[i].src == BT_MESH_ADDR_UNASSIGNED) {
            bt_mesh.rpl[i].src = src;
            return &bt_mesh.rpl[i];
        }
    }

    return NULL;
}

static int rpl_set(const char *name)
{
    struct net_buf_simple *buf = NULL;
    struct bt_mesh_rpl *entry = NULL;
    struct rpl_val rpl = {0};
    char get[16] = {'\0'};
    bool exist = false;
    size_t length = 0U;
    int err = 0;
    int i;

    BT_DBG("%s", __func__);

    buf = bt_mesh_get_core_settings_item(name);
    if (!buf) {
        return 0;
    }

    length = buf->len;

    for (i = 0; i < length / SETTINGS_ITEM_SIZE; i++) {
        u16_t src = simple_pull_le16(buf);
        sprintf(get, "mesh/rpl/%04x", src);

        err = bt_mesh_load_core_settings(get, (u8_t *)&rpl, sizeof(rpl), &exist);
        if (err) {
            BT_ERR("%s, Failed to load RPL %s, reset RPL", __func__, get);
            bt_mesh_rpl_reset();
            goto free;
        }

        if (exist == false) {
            continue;
        }

        entry = rpl_find(src);
        if (!entry) {
            entry = rpl_alloc(src);
            if (!entry) {
                BT_ERR("%s, No space for a new RPL 0x%04x", __func__, src);
                err = -ENOMEM;
                goto free;
            }
        }

        BT_DBG("Restored RPL 0x%04x: Seq 0x%06x, old_iv %u", src, rpl.seq, rpl.old_iv);
        entry->src = src;
        entry->seq = rpl.seq;
        entry->old_iv = rpl.old_iv;
    }

free:
    free(buf);
    return err;
}

static struct bt_mesh_subnet *subnet_alloc(u16_t net_idx)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(bt_mesh.sub); i++) {
        if (bt_mesh.sub[i].net_idx == BT_MESH_KEY_UNUSED) {
            bt_mesh.sub[i].net_idx = net_idx;
            return &bt_mesh.sub[i];
        }
    }

    return NULL;
}

static int net_key_set(const char *name)
{
    struct net_buf_simple *buf = NULL;
    struct bt_mesh_subnet *sub = NULL;
    struct net_key_val key = {0};
    char get[16] = {'\0'};
    bool exist = false;
    size_t length = 0U;
    int err = 0;
    int i;

    BT_DBG("%s", __func__);

    buf = bt_mesh_get_core_settings_item(name);
    if (!buf) {
        return 0;
    }

    length = buf->len;

    for (i = 0; i < length / sizeof(u16_t); i++) {
        u16_t net_idx = simple_pull_le16(buf);
        sprintf(get, "mesh/nk/%04x", net_idx);

        err = bt_mesh_load_core_settings(get, (u8_t *)&key, sizeof(key), &exist);
        if (err) {
            BT_ERR("%s, Failed to load NetKey %s", __func__, get);
            goto free;
        }

        if (exist == false) {
            continue;
        }

        sub = bt_mesh_subnet_get(net_idx);
        if (!sub) {
            sub = subnet_alloc(net_idx);
            if (!sub) {
                BT_ERR("%s, No space for a new subnet 0x%03x", __func__, net_idx);
                err = -ENOMEM;
                goto free;
            }
        }

        BT_DBG("Restored NetKey Index 0x%03x", net_idx);
        sub->net_idx = net_idx;
        sub->kr_flag = key.kr_flag;
        sub->kr_phase = key.kr_phase;
        memcpy(sub->keys[0].net, &key.val[0], 16);
        memcpy(sub->keys[1].net, &key.val[1], 16);
    }

free:
    free(buf);
    return err;
}

static int app_key_set(const char *name)
{
    struct bt_mesh_app_key *app = NULL;
    struct bt_mesh_subnet *sub = NULL;
    struct net_buf_simple *buf = NULL;
    struct app_key_val key = {0};
    char get[16] = {'\0'};
    bool exist = false;
    size_t length = 0U;
    int err = 0;
    int i;

    BT_DBG("%s", __func__);

    buf = bt_mesh_get_core_settings_item(name);
    if (!buf) {
        return 0;
    }

    length = buf->len;

    for (i = 0; i < length / sizeof(u16_t); i++) {
        u16_t app_idx = simple_pull_le16(buf);
        sprintf(get, "mesh/ak/%04x", app_idx);

        err = bt_mesh_load_core_settings(get, (u8_t *)&key, sizeof(key), &exist);
        if (err) {
            BT_ERR("%s, Failed to load AppKey %s", __func__, get);
            goto free;
        }

        if (exist == false) {
            continue;
        }

        sub = bt_mesh_subnet_get(key.net_idx);
        if (!sub) {
            BT_ERR("%s, Failed to find subnet 0x%03x", __func__, key.net_idx);
            err = -ENOENT;
            goto free;
        }

        app = bt_mesh_app_key_find(app_idx);
        if (!app) {
            app = bt_mesh_app_key_alloc(app_idx);
            if (!app) {
                BT_ERR("%s, No space for a new app key 0x%03x", __func__, app_idx);
                err = -ENOMEM;
                goto free;
            }
        }

        BT_DBG("Restored AppKey Index 0x%03x", app_idx);
        app->net_idx = key.net_idx;
        app->app_idx = app_idx;
        app->updated = key.updated;
        memcpy(app->keys[0].val, key.val[0], 16);
        memcpy(app->keys[1].val, key.val[1], 16);
        bt_mesh_app_id(app->keys[0].val, &app->keys[0].id);
        bt_mesh_app_id(app->keys[1].val, &app->keys[1].id);
    }

free:
    free(buf);
    return err;
}

static int hb_pub_set(const char *name)
{
    struct bt_mesh_hb_pub *hb_pub = bt_mesh_hb_pub_get();
    struct hb_pub_val hb_val = {0};
    bool exist = false;
    int err = 0;

    BT_DBG("%s", __func__);

    if (!hb_pub) {
        BT_ERR("%s, NULL cfg hb pub", __func__);
        return -EINVAL;
    }

    err = bt_mesh_load_core_settings(name, (u8_t *)&hb_val, sizeof(hb_val), &exist);
    if (err) {
        BT_DBG("%s, Cleared heartbeat publication", __func__);
        hb_pub->dst = BT_MESH_ADDR_UNASSIGNED;
        hb_pub->count = 0U;
        hb_pub->ttl = 0U;
        hb_pub->period = 0U;
        hb_pub->feat = 0U;
        return 0;
    }

    if (exist == false) {
        return 0;
    }

    hb_pub->dst = hb_val.dst;
    hb_pub->period = hb_val.period;
    hb_pub->ttl = hb_val.ttl;
    hb_pub->feat = hb_val.feat;
    hb_pub->net_idx = hb_val.net_idx;
    if (hb_val.indefinite) {
        hb_pub->count = 0xffff;
    } else {
        hb_pub->count = 0U;
    }

    BT_DBG("Restored heartbeat publication, dst 0x%04x", hb_pub->dst);

    return 0;
}

static int cfg_set(const char *name)
{
    struct bt_mesh_cfg_srv *cfg = bt_mesh_cfg_get();
    struct cfg_val val = {0};
    bool exist = false;
    int err = 0;

    BT_DBG("%s", __func__);

    if (!cfg) {
        BT_ERR("%s, NULL cfg", __func__);
        stored_cfg.valid = false;
        return -EINVAL;
    }

    err = bt_mesh_load_core_settings(name, (u8_t *)&val, sizeof(val), &exist);
    if (err) {
        BT_DBG("%s, Cleared configuration", __func__);
        stored_cfg.valid = false;
        return 0;
    }

    if (exist == false) {
        return 0;
    }

    memcpy(&stored_cfg.cfg, &val, sizeof(val));
    stored_cfg.valid = true;
    BT_DBG("Restored configuration state");
    return 0;
}

static int model_set_bind(bool vnd, struct bt_mesh_model *model, u16_t model_key)
{
    char name[16] = {'\0'};
    bool exist = false;
    int err = 0;
    int i;

    /* Start with empty array regardless of cleared or set value */
    for (i = 0; i < ARRAY_SIZE(model->keys); i++) {
        model->keys[i] = BT_MESH_KEY_UNUSED;
    }

    sprintf(name, "mesh/%s/%04x/b", vnd ? "v" : "s", model_key);
    err = bt_mesh_load_core_settings(name, (u8_t *)model->keys, sizeof(model->keys), &exist);
    if (err) {
        BT_ERR("%s, Failed to get model bind keys", __func__);
        return -EIO;
    }

    return 0;
}

static int model_set_sub(bool vnd, struct bt_mesh_model *model, u16_t model_key)
{
    char name[16] = {'\0'};
    bool exist = false;
    int err = 0;
    int i;

    /* Start with empty array regardless of cleared or set value */
    for (i = 0; i < ARRAY_SIZE(model->groups); i++) {
        model->groups[i] = BT_MESH_ADDR_UNASSIGNED;
    }

    sprintf(name, "mesh/%s/%04x/s", vnd ? "v" : "s", model_key);
    err = bt_mesh_load_core_settings(name, (u8_t *)model->groups, sizeof(model->groups), &exist);
    if (err) {
        BT_ERR("%s, Failed to get model subscriptions", __func__);
        return -EIO;
    }

    return 0;
}

static int model_set_pub(bool vnd, struct bt_mesh_model *model, u16_t model_key)
{
    struct mod_pub_val pub = {0};
    char name[16] = {'\0'};
    bool exist = false;
    int err = 0;

    if (!model->pub) {
        BT_WARN("%s, Model has no publication context", __func__);
        return 0;
    }

    sprintf(name, "mesh/%s/%04x/p", vnd ? "v" : "s", model_key);
    err = bt_mesh_load_core_settings(name, (u8_t *)&pub, sizeof(pub), &exist);
    if (err) {
        BT_WARN("%s, Cleared model publication", __func__);
        model->pub->addr = BT_MESH_KEY_UNUSED;
        model->pub->key = 0U;
        model->pub->cred = 0U;
        model->pub->ttl = 0U;
        model->pub->period = 0U;
        model->pub->retransmit = 0U;
        model->pub->count = 0U;
        return 0;
    }

    if (exist == false) {
        return 0;
    }

    model->pub->addr = pub.addr;
    model->pub->key = pub.key;
    model->pub->cred = pub.cred;
    model->pub->ttl = pub.ttl;
    model->pub->period = pub.period;
    model->pub->retransmit = pub.retransmit;
    model->pub->count = 0U;

    BT_INFO("Restored model publication, pub_addr 0x%04x, app_idx 0x%03x",
           pub.addr, pub.key);

    return 0;
}

static int model_set(bool vnd, const char *name)
{
    struct bt_mesh_model *model = NULL;
    struct net_buf_simple *buf = NULL;
    u8_t elem_idx = 0U, mod_idx = 0U;
    size_t length = 0U;
    int err = 0;
    int i;

    BT_DBG("%s", __func__);

    buf = bt_mesh_get_core_settings_item(name);
    if (!buf) {
        return 0;
    }

    length = buf->len;

    for (i = 0; i < length / SETTINGS_ITEM_SIZE; i++) {
        u16_t model_key = simple_pull_le16(buf);
        elem_idx = BLE_MESH_GET_ELEM_IDX(model_key);
        mod_idx = BLE_MESH_GET_MODEL_IDX(model_key);

        model = bt_mesh_model_get(vnd, elem_idx, mod_idx);
        if (!model) {
            BT_ERR("%s, Failed to get %s model, elem_idx %u mod_idx %u",
                   __func__, vnd ? "vnd" : "sig", elem_idx, mod_idx);
            err = -ENOENT;
            goto free;
        }

        err = model_set_bind(vnd, model, model_key);
        if (err) {
            BT_ERR("%s, model_set_bind fail", __func__);
            goto free;
        }

        err = model_set_sub(vnd, model, model_key);
        if (err) {
            BT_ERR("%s, model_set_sub fail", __func__);
            goto free;
        }

        err = model_set_pub(vnd, model, model_key);
        if (err) {
            BT_ERR("%s, model_set_pub fail", __func__);
            goto free;
        }
    }

free:
    free(buf);
    return err;
}

static int sig_mod_set(const char *name)
{
    return model_set(false, name);
}

static int vnd_mod_set(const char *name)
{
    return model_set(true, name);
}

/* 从flash中还原配置*/
const struct bt_mesh_setting {
    const char *name;
    int (*func)(const char *name);
} settings[] = {
    { "mesh/net",      net_set       },
    { "mesh/iv",       iv_set        },
    { "mesh/seq",      seq_set       },
    { "mesh/rpl",      rpl_set       },
    { "mesh/netkey",   net_key_set   },
    { "mesh/appkey",   app_key_set   },
    { "mesh/hb_pub",   hb_pub_set    },
    { "mesh/cfg",      cfg_set       },
    { "mesh/sig",      sig_mod_set   },
    { "mesh/vnd",      vnd_mod_set   },
};

/**
 * 从flash中还原配置
 * For Provisioner, the load operation needs the following actions:
 * net_set:     Not needed
 * iv_set:      Need, although Provisioner will do some initialization of IV Index
 *              during startup, but we need to restore the last IV Index status
 * seq_set:     Need, restore the previous sequence number
 * rpl_set:     Need, restore the previous Replay Protection List
 * net_key_set: Need, restore the previous network keys
 * app_key_set: Need, restore the previous application keys
 * hb_pub_set:  Not needed currently
 * cfg_set:     Not needed currently
 * sig_mod_set: Need, restore SIG models related info (app, sub, pub)
 * vnd_mod_set: Need, restore vendor models related info (app, sub, pub)
 */
int settings_core_load(void)
{
    int i;

    BT_DBG("%s", __func__);

    for (i = 0; i < ARRAY_SIZE(settings); i++) {
        settings[i].func(settings[i].name);
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////
static int subnet_init(struct bt_mesh_subnet *sub)
{
    int err = 0;

    err = bt_mesh_net_keys_create(&sub->keys[0], sub->keys[0].net);
    if (err) {
        BT_ERR("%s, Unable to generate keys for subnet", __func__);
        return -EIO;
    }

    if (sub->kr_phase != BT_MESH_KR_NORMAL) {
        err = bt_mesh_net_keys_create(&sub->keys[1], sub->keys[1].net);
        if (err) {
            BT_ERR("%s, Unable to generate keys for subnet", __func__);
            (void)memset(&sub->keys[0], 0, sizeof(sub->keys[0]));
            return -EIO;
        }
    }

    if (IS_ENABLED(CONFIG_BT_MESH_GATT_PROXY)) {
        sub->node_id = BT_MESH_NODE_IDENTITY_STOPPED;
    } else {
        sub->node_id = BT_MESH_NODE_IDENTITY_NOT_SUPPORTED;
    }

    /* Make sure we have valid beacon data to be sent */
    bt_mesh_net_beacon_update(sub);

    return 0;
}

static void commit_model(struct bt_mesh_model *model, struct bt_mesh_elem *elem,
                         bool vnd, bool primary, void *user_data)
{
    if (model->pub && model->pub->update &&
            model->pub->addr != BT_MESH_KEY_UNUSED) {
        s32_t ms = bt_mesh_model_pub_period_get(model);
        if (ms) {
            BT_DBG("Starting publish timer (period %u ms)", ms);
            k_delayed_work_submit(&model->pub->timer, ms);
        }
    }
}

int settings_core_commit(void)
{
    struct bt_mesh_subnet *sub = NULL;
    int err = 0;
    int i;

    BT_INFO("sub[0].net_idx 0x%03x", bt_mesh.sub[0].net_idx);

    if (bt_mesh.sub[0].net_idx == BT_MESH_KEY_UNUSED) {
        /* Nothing to do since we're not yet provisioned */
        return 0;
    }

    if (IS_ENABLED(CONFIG_BT_MESH_PB_GATT)) {
        bt_mesh_proxy_prov_disable();
    }

    for (i = 0; i < ARRAY_SIZE(bt_mesh.sub); i++) {
        sub = &bt_mesh.sub[i];

        if (sub->net_idx == BT_MESH_KEY_UNUSED) {
            continue;
        }

        err = subnet_init(sub);
        if (err) {
            BT_ERR("%s, Failed to init subnet 0x%03x", __func__, sub->net_idx);
        }
    }

    if (bt_mesh.ivu_duration < BT_MESH_IVU_MIN_HOURS) {
        k_delayed_work_submit(&bt_mesh.ivu_timer, BT_MESH_IVU_TIMEOUT);
    }

    bt_mesh_model_foreach(commit_model, NULL);

    struct bt_mesh_hb_pub *hb_pub = NULL;
    struct bt_mesh_cfg_srv *cfg = NULL;

    hb_pub = bt_mesh_hb_pub_get();
    if (hb_pub && hb_pub->dst != BT_MESH_KEY_UNUSED &&
            hb_pub->count && hb_pub->period) {
        BT_DBG("Starting heartbeat publication");
        k_work_submit(&hb_pub->timer.work);
    }

    cfg = bt_mesh_cfg_get();
    if (cfg && stored_cfg.valid) {
        cfg->net_transmit = stored_cfg.cfg.net_transmit;
        cfg->relay = stored_cfg.cfg.relay;
        cfg->relay_retransmit = stored_cfg.cfg.relay_retransmit;
        cfg->beacon = stored_cfg.cfg.beacon;
        cfg->gatt_proxy = stored_cfg.cfg.gatt_proxy;
        cfg->frnd = stored_cfg.cfg.frnd;
        cfg->default_ttl = stored_cfg.cfg.default_ttl;
    }

    atomic_set_bit(bt_mesh.flags, BT_MESH_VALID);
    bt_mesh_net_start();

    return 0;
}

/* Pending flags that use K_NO_WAIT as the storage timeout */
#define NO_WAIT_PENDING_BITS (BIT(BT_MESH_NET_PENDING) |       \
                              BIT(BT_MESH_IV_PENDING) |        \
                              BIT(BT_MESH_SEQ_PENDING))

/* Pending flags that use CONFIG_BLE_MESH_STORE_TIMEOUT */
#define GENERIC_PENDING_BITS (BIT(BT_MESH_KEYS_PENDING) |      \
                              BIT(BT_MESH_HB_PUB_PENDING) |    \
                              BIT(BT_MESH_CFG_PENDING) |       \
                              BIT(BT_MESH_MOD_PENDING))

static void schedule_store(int flag)
{
    s32_t timeout = 0, remaining = 0;

    atomic_set_bit(bt_mesh.flags, flag); //挂起霜要存储操作的位。

    if (bt_mesh_atomic_get(bt_mesh.flags) & NO_WAIT_PENDING_BITS) {
        timeout = K_NO_WAIT;
    } else if (atomic_test_bit(bt_mesh.flags, BT_MESH_RPL_PENDING) &&
               (!(bt_mesh_atomic_get(bt_mesh.flags) & GENERIC_PENDING_BITS) ||
                (CONFIG_BT_MESH_RPL_STORE_TIMEOUT <
                 CONFIG_BT_MESH_STORE_TIMEOUT))) {
        timeout = K_SECONDS(CONFIG_BT_MESH_RPL_STORE_TIMEOUT);
    } else {
        timeout = K_SECONDS(CONFIG_BT_MESH_STORE_TIMEOUT);
    }

    remaining = k_delayed_work_remaining_get(&pending_store);
    if (remaining && remaining < timeout) {
        BT_DBG("Not rescheduling due to existing earlier deadline");
        return;
    }

    BT_DBG("Waiting %d seconds", timeout / MSEC_PER_SEC);

    if (timeout) {
        k_delayed_work_submit(&pending_store, timeout);
    } else {
        k_work_submit(&pending_store.work);
    }
}

static void clear_iv(void)
{
    BT_DBG("Clearing IV");
    bt_mesh_save_core_settings("mesh/iv", NULL, 0);
}

static void clear_net(void)
{
    BT_DBG("Clearing Network");
    bt_mesh_save_core_settings("mesh/net", NULL, 0);
}

static void store_pending_net(void)
{
    struct net_val net = {0};

    BT_DBG("addr 0x%04x DevKey %s", bt_mesh_primary_addr(),
           bt_hex(bt_mesh.dev_key, 16));

    net.primary_addr = bt_mesh_primary_addr();
    memcpy(net.dev_key, bt_mesh.dev_key, 16);

    bt_mesh_save_core_settings("mesh/net", (const u8_t *)&net, sizeof(net));
}

void bt_mesh_store_net(void)
{
    schedule_store(BT_MESH_NET_PENDING);
}

static void store_pending_iv(void)
{
    struct iv_val iv = {0};

    iv.iv_index = bt_mesh.iv_index;
    iv.iv_update = atomic_test_bit(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS);
    iv.iv_duration = bt_mesh.ivu_duration;

    bt_mesh_save_core_settings("mesh/iv", (const u8_t *)&iv, sizeof(iv));
}

void bt_mesh_store_iv(bool only_duration)
{
    schedule_store(BT_MESH_IV_PENDING);

    if (!only_duration) {
        /* Always update Seq whenever IV changes */
        schedule_store(BT_MESH_SEQ_PENDING);
    }
}

void bt_mesh_clear_iv(void)
{
    clear_iv();
}

static void store_pending_seq(void)
{
    struct seq_val seq = {0};

    seq.val[0] = bt_mesh.seq;
    seq.val[1] = bt_mesh.seq >> 8;
    seq.val[2] = bt_mesh.seq >> 16;

    bt_mesh_save_core_settings("mesh/seq", (const u8_t *)&seq, sizeof(seq));
}

void bt_mesh_store_seq(void)
{
    if (CONFIG_BT_MESH_SEQ_STORE_RATE &&
            (bt_mesh.seq % CONFIG_BT_MESH_SEQ_STORE_RATE)) {
        return;
    }

    schedule_store(BT_MESH_SEQ_PENDING);
}

void bt_mesh_clear_seq(void)
{
    bt_mesh_save_core_settings("mesh/seq", NULL, 0);
}

static void store_rpl(struct bt_mesh_rpl *entry)
{
    struct rpl_val rpl = {0};
    char name[16] = {'\0'};
    int err = 0;

    BT_DBG("src 0x%04x seq 0x%06x old_iv %u", entry->src, entry->seq, entry->old_iv);

    rpl.seq = entry->seq;
    rpl.old_iv = entry->old_iv;

    sprintf(name, "mesh/rpl/%04x", entry->src);
    err = bt_mesh_save_core_settings(name, (const u8_t *)&rpl, sizeof(rpl));
    if (err) {
        BT_ERR("%s, Failed to save RPL %s", __func__, name);
        return;
    }

    err = bt_mesh_add_core_settings_item("mesh/rpl", entry->src);
    if (err) {
        BT_ERR("%s, Failed to add 0x%04x to mesh/rpl", __func__, entry->src);
    }

    return;
}

static void clear_rpl(void)
{
    struct net_buf_simple *buf = NULL;
    char name[16] = {'\0'};
    size_t length = 0U;
    u16_t src = 0U;
    int i;

    BT_DBG("%s", __func__);

    bt_mesh_rpl_clear();

    buf = bt_mesh_get_core_settings_item("mesh/rpl");
    if (!buf) {
        BT_WARN("%s, Erase RPL", __func__);
        bt_mesh_save_core_settings("mesh/rpl", NULL, 0);
        return;
    }

    length = buf->len;

    for (i = 0; i < length / SETTINGS_ITEM_SIZE; i++) {
        src = simple_pull_le16(buf);
        sprintf(name, "mesh/rpl/%04x", src);
        bt_mesh_save_core_settings(name, NULL, 0);
    }

    bt_mesh_save_core_settings("mesh/rpl", NULL, 0);

    free(buf);
    return;
}

static void store_pending_rpl(void)
{
    int i;

    BT_DBG("%s", __func__);

    for (i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        struct bt_mesh_rpl *rpl = &bt_mesh.rpl[i];

        if (rpl->store) {
            rpl->store = false;
            store_rpl(rpl);
        }
    }
}

static void store_pending_hb_pub(void)
{
    struct bt_mesh_hb_pub *hb_pub = bt_mesh_hb_pub_get();
    struct hb_pub_val val = {0};

    if (!hb_pub) {
        BT_WARN("%s, NULL hb_pub", __func__);
        return;
    }

    val.indefinite = (hb_pub->count = 0xffff);
    val.dst = hb_pub->dst;
    val.period = hb_pub->period;
    val.ttl = hb_pub->ttl;
    val.feat = hb_pub->feat;
    val.net_idx = hb_pub->net_idx;

    bt_mesh_save_core_settings("mesh/hb_pub", (const u8_t *)&val, sizeof(val));
}

static void store_pending_cfg(void)
{
    struct bt_mesh_cfg_srv *cfg = bt_mesh_cfg_get();
    struct cfg_val val = {0};

    if (!cfg) {
        BT_WARN("%s, NULL cfg", __func__);
        return;
    }

    val.net_transmit = cfg->net_transmit;
    val.relay = cfg->relay;
    val.relay_retransmit = cfg->relay_retransmit;
    val.beacon = cfg->beacon;
    val.gatt_proxy = cfg->gatt_proxy;
    val.frnd = cfg->frnd;
    val.default_ttl = cfg->default_ttl;

    bt_mesh_save_core_settings("mesh/cfg", (const u8_t *)&val, sizeof(val));
}

static void clear_cfg(void)
{
    BT_DBG("Clearing configuration");
    bt_mesh_save_core_settings("mesh/cfg", NULL, 0);
}

static void clear_app_key(u16_t app_idx)
{
    char name[16] = {'\0'};
    int err = 0;

    BT_DBG("AppKeyIndex 0x%03x", app_idx);

    sprintf(name, "mesh/ak/%04x", app_idx);
    bt_mesh_save_core_settings(name, NULL, 0);

    err = bt_mesh_remove_core_settings_item("mesh/appkey", app_idx);
    if (err) {
        BT_ERR("%s, Failed to remove 0x%04x from mesh/appkey", __func__, app_idx);
    }

    return;
}

static void clear_net_key(u16_t net_idx)
{
    char name[16] = {'\0'};
    int err = 0;

    BT_DBG("NetKeyIndex 0x%03x", net_idx);

    sprintf(name, "mesh/nk/%04x", net_idx);
    bt_mesh_save_core_settings(name, NULL, 0);

    err = bt_mesh_remove_core_settings_item("mesh/netkey", net_idx);
    if (err) {
        BT_ERR("%s, Failed to remove 0x%04x from mesh/netkey", __func__, net_idx);
    }

    return;
}

static void store_net_key(struct bt_mesh_subnet *sub)
{
    struct net_key_val key = {0};
    char name[16] = {'\0'};
    int err = 0;

    BT_DBG("NetKeyIndex 0x%03x NetKey %s", sub->net_idx,
           bt_hex(sub->keys[0].net, 16));

    memcpy(&key.val[0], sub->keys[0].net, 16);
    memcpy(&key.val[1], sub->keys[1].net, 16);
    key.kr_flag = sub->kr_flag;
    key.kr_phase = sub->kr_phase;

    sprintf(name, "mesh/nk/%04x", sub->net_idx);
    err = bt_mesh_save_core_settings(name, (const u8_t *)&key, sizeof(key));
    if (err) {
        BT_ERR("%s, Failed to save NetKey %s", __func__, name);
        return;
    }

    err = bt_mesh_add_core_settings_item("mesh/netkey", sub->net_idx);
    if (err) {
        BT_ERR("%s, Failed to add 0x%04x to mesh/netkey", __func__, sub->net_idx);
    }

    return;
}

static void store_app_key(struct bt_mesh_app_key *app)
{
    struct app_key_val key = {0};
    char name[16] = {'\0'};
    int err = 0;

    key.net_idx = app->net_idx;
    key.updated = app->updated;
    memcpy(key.val[0], app->keys[0].val, 16);
    memcpy(key.val[1], app->keys[1].val, 16);

    sprintf(name, "mesh/ak/%04x", app->app_idx);
    err = bt_mesh_save_core_settings(name, (const u8_t *)&key, sizeof(key));
    if (err) {
        BT_ERR("%s, Failed to save AppKey %s", __func__, name);
        return;
    }

    err = bt_mesh_add_core_settings_item("mesh/appkey", app->app_idx);
    if (err) {
        BT_ERR("%s, Failed to add 0x%04x to mesh/appkey", __func__, app->app_idx);
    }

    return;
}

static void store_pending_keys(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(key_updates); i++) {
        struct key_update *update = &key_updates[i];

        if (!update->valid) {
            continue;
        }

        if (update->clear) {
            if (update->app_key) {
                clear_app_key(update->key_idx);
            } else {
                clear_net_key(update->key_idx);
            }
        } else {
            if (update->app_key) {
                struct bt_mesh_app_key *key = NULL;
                key = bt_mesh_app_key_find(update->key_idx);
                if (key) {
                    store_app_key(key);
                } else {
                    BT_WARN("AppKeyIndex 0x%03x not found", update->key_idx);
                }
            } else {
                struct bt_mesh_subnet *sub = NULL;
                sub = bt_mesh_subnet_get(update->key_idx);
                if (sub) {
                    store_net_key(sub);
                } else {
                    BT_WARN("NetKeyIndex 0x%03x not found", update->key_idx);
                }
            }
        }

        update->valid = 0U;
    }
}

static void store_pending_mod_bind(struct bt_mesh_model *model, bool vnd)
{
    char name[16] = {'\0'};
    u16_t model_key = 0U;
    int err = 0;

    model_key = BLE_MESH_GET_MODEL_KEY(model->elem_idx, model->mod_idx);

    sprintf(name, "mesh/%s/%04x/b", vnd ? "v" : "s", model_key);

    // if (CONFIG_BLE_MESH_NODE && bt_mesh_is_node() &&
    //         !bt_mesh_is_provisioned()) {
    //     bt_mesh_save_core_settings(name, NULL, 0);
    //     return;
    // }

    err = bt_mesh_save_core_settings(name, (const u8_t *)model->keys, sizeof(model->keys));
    if (err) {
        BT_ERR("%s, Failed to save %s", __func__, name);
        return;
    }

    err = bt_mesh_add_core_settings_item(vnd ? "mesh/vnd" : "mesh/sig", model_key);
    if (err) {
        BT_ERR("%s, Failed to add 0x%04x to %s", __func__, model_key,
               vnd ? "mesh/vnd" : "mesh/sig");
    }

    return;
}

static void store_pending_mod_sub(struct bt_mesh_model *model, bool vnd)
{
    char name[16] = {'\0'};
    u16_t model_key = 0U;
    int err = 0;

    model_key = BLE_MESH_GET_MODEL_KEY(model->elem_idx, model->mod_idx);

    sprintf(name, "mesh/%s/%04x/s", vnd ? "v" : "s", model_key);

    // if (CONFIG_BLE_MESH_NODE && bt_mesh_is_node() &&
    //         !bt_mesh_is_provisioned()) {
    //     bt_mesh_save_core_settings(name, NULL, 0);
    //     return;
    // }

    err = bt_mesh_save_core_settings(name, (const u8_t *)model->groups, sizeof(model->groups));
    if (err) {
        BT_ERR("%s, Failed to save %s", __func__, name);
        return;
    }

    err = bt_mesh_add_core_settings_item(vnd ? "mesh/vnd" : "mesh/sig", model_key);
    if (err) {
        BT_ERR("%s, Failed to add 0x%04x to %s", __func__, model_key,
               vnd ? "mesh/vnd" : "mesh/sig");
    }

    return;
}

static void store_pending_mod_pub(struct bt_mesh_model *model, bool vnd)
{
    struct mod_pub_val pub = {0};
    char name[16] = {'\0'};
    u16_t model_key = 0U;
    int err = 0;

    if (!model->pub) {
        BT_WARN("%s, No model publication to store", __func__);
        return;
    }

    pub.addr = model->pub->addr;
    pub.key = model->pub->key;
    pub.ttl = model->pub->ttl;
    pub.retransmit = model->pub->retransmit;
    pub.period = model->pub->period;
    pub.period_div = model->pub->period_div;
    pub.cred = model->pub->cred;

    model_key = BLE_MESH_GET_MODEL_KEY(model->elem_idx, model->mod_idx);

    sprintf(name, "mesh/%s/%04x/p", vnd ? "v" : "s", model_key);

    // if (CONFIG_BLE_MESH_NODE && bt_mesh_is_node() &&
    //         !bt_mesh_is_provisioned()) {
    //     bt_mesh_save_core_settings(name, NULL, 0);
    //     return;
    // }

    err = bt_mesh_save_core_settings(name, (const u8_t *)&pub, sizeof(pub));
    if (err) {
        BT_ERR("%s, Failed to save %s", __func__, name);
        return;
    }

    err = bt_mesh_add_core_settings_item(vnd ? "mesh/vnd" : "mesh/sig", model_key);
    if (err) {
        BT_ERR("%s, Failed to add 0x%04x to %s", __func__, model_key,
               vnd ? "mesh/vnd" : "mesh/sig");
    }

    return;
}

static void store_pending_mod(struct bt_mesh_model *model,
                              struct bt_mesh_elem *elem, bool vnd,
                              bool primary, void *user_data)
{
    if (!model->flags) {
        return;
    }

    if (model->flags & BT_MESH_MOD_BIND_PENDING) {
        model->flags &= ~BT_MESH_MOD_BIND_PENDING;
        store_pending_mod_bind(model, vnd);
    }

    if (model->flags & BT_MESH_MOD_SUB_PENDING) {
        model->flags &= ~BT_MESH_MOD_SUB_PENDING;
        store_pending_mod_sub(model, vnd);
    }

    if (model->flags & BT_MESH_MOD_PUB_PENDING) {
        model->flags &= ~BT_MESH_MOD_PUB_PENDING;
        store_pending_mod_pub(model, vnd);
    }
}

/* 存储相应挂起位对应的配置数据 */
static void store_pending(struct ble_npl_event *ev)
{
    BT_DBG("%s", __func__);

    if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_RPL_PENDING)) {
        if (atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
            store_pending_rpl();
        } else {
            clear_rpl();
        }
    }

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_KEYS_PENDING)) {
		store_pending_keys();
	}

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_NET_PENDING)) {
		if (atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
			store_pending_net();
		} else {
			clear_net();
		}
	}

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_IV_PENDING)) {
		if (atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
			store_pending_iv();
		} else {
			clear_iv();
		}
	}

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_SEQ_PENDING)) {
		store_pending_seq();
	}

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_HB_PUB_PENDING)) {
		store_pending_hb_pub();
	}

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_CFG_PENDING)) {
		if (atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
			store_pending_cfg();
		} else {
			clear_cfg();
		}
	}

	if (atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_MOD_PENDING)) {
		bt_mesh_model_foreach(store_pending_mod, NULL);
        // bt_mesh_save_core_settings("mesh/sig", NULL, 0);
        // bt_mesh_save_core_settings("mesh/vnd", NULL, 0);
	}

}

void bt_mesh_store_rpl(struct bt_mesh_rpl *entry)
{
    entry->store = true;
    schedule_store(BT_MESH_RPL_PENDING);
}

static struct key_update *key_update_find(bool app_key, u16_t key_idx,
        struct key_update **free_slot)
{
    struct key_update *match = NULL;
    int i;

    *free_slot = NULL;

    for (i = 0; i < ARRAY_SIZE(key_updates); i++) {
        struct key_update *update = &key_updates[i];

        if (!update->valid) {
            *free_slot = update;
            continue;
        }

        if (update->app_key != app_key) {
            continue;
        }

        if (update->key_idx == key_idx) {
            match = update;
        }
    }

    return match;
}

void bt_mesh_store_subnet(struct bt_mesh_subnet *sub)
{
    struct key_update *free_slot = NULL;
    struct key_update *update = NULL;

    BT_DBG("NetKeyIndex 0x%03x", sub->net_idx);

    update = key_update_find(false, sub->net_idx, &free_slot);
    if (update) {
        update->clear = 0U;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if (!free_slot) {
        store_net_key(sub);
        return;
    }

    free_slot->valid = 1U;
    free_slot->key_idx = sub->net_idx;
    free_slot->app_key = 0U;
    free_slot->clear = 0U;

    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_store_app_key(struct bt_mesh_app_key *key)
{
    struct key_update *free_slot = NULL;
    struct key_update *update = NULL;

    BT_DBG("AppKeyIndex 0x%03x", key->app_idx);

    update = key_update_find(true, key->app_idx, &free_slot);
    if (update) {
        update->clear = 0U;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if (!free_slot) {
        store_app_key(key);
        return;
    }

    free_slot->valid = 1U;
    free_slot->key_idx = key->app_idx;
    free_slot->app_key = 1U;
    free_slot->clear = 0U;

    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_store_hb_pub(void)
{
    schedule_store(BT_MESH_HB_PUB_PENDING);
}

void bt_mesh_store_cfg(void)
{
    schedule_store(BT_MESH_CFG_PENDING);
}

void bt_mesh_clear_net(void)
{
    schedule_store(BT_MESH_NET_PENDING);
    schedule_store(BT_MESH_IV_PENDING);
    schedule_store(BT_MESH_CFG_PENDING);
}

void bt_mesh_clear_subnet(struct bt_mesh_subnet *sub)
{
    struct key_update *free_slot = NULL;
    struct key_update *update = NULL;

    BT_DBG("NetKeyIndex 0x%03x", sub->net_idx);

    update = key_update_find(false, sub->net_idx, &free_slot);
    if (update) {
        update->clear = 1U;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if (!free_slot) {
        clear_net_key(sub->net_idx);
        return;
    }

    free_slot->valid = 1U;
    free_slot->key_idx = sub->net_idx;
    free_slot->app_key = 0U;
    free_slot->clear = 1U;

    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_clear_app_key(struct bt_mesh_app_key *key)
{
    struct key_update *free_slot = NULL;
    struct key_update *update = NULL;

    BT_DBG("AppKeyIndex 0x%03x", key->app_idx);

    update = key_update_find(true, key->app_idx, &free_slot);
    if (update) {
        update->clear = 1U;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if (!free_slot) {
        clear_app_key(key->app_idx);
        return;
    }

    free_slot->valid = 1U;
    free_slot->key_idx = key->app_idx;
    free_slot->app_key = 1U;
    free_slot->clear = 1U;

    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_clear_rpl(void)
{
    schedule_store(BT_MESH_RPL_PENDING);
}

void bt_mesh_store_mod_bind(struct bt_mesh_model *model)
{
    model->flags |= BT_MESH_MOD_BIND_PENDING;
    schedule_store(BT_MESH_MOD_PENDING);
}

void bt_mesh_store_mod_sub(struct bt_mesh_model *model)
{
    model->flags |= BT_MESH_MOD_SUB_PENDING;
    schedule_store(BT_MESH_MOD_PENDING);
}

void bt_mesh_store_mod_pub(struct bt_mesh_model *model)
{
    model->flags |= BT_MESH_MOD_PUB_PENDING;
    schedule_store(BT_MESH_MOD_PENDING);
}

///////////////////////////////////////////
int settings_core_init(void)
{
    BT_DBG("%s", __func__);

    k_delayed_work_init(&pending_store, store_pending);

    return 0;
}

void bt_mesh_settings_init(void)
{
    BT_DBG("%s", __func__);

    bt_mesh_settings_foreach();
}

int settings_core_deinit(void)
{
    k_delayed_work_cancel(&pending_store);

    return 0;
}

int bt_mesh_settings_deinit(void)
{
    bt_mesh_settings_deforeach();

    return 0;
}

// #endif /* CONFIG_BLE_MESH_SETTINGS */
