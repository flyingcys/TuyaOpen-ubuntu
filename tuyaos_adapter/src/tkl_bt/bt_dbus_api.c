#include <glib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include "adapter.h"
#include "device.h"
#include "logger.h"
#include "agent.h"
#include "application.h"
#include "advertisement.h"
#include "utility.h"
#include "parser.h"

#include <pthread.h>
#include "tkl_bluetooth.h"

#define TAG "BT_DBUS_API"

static GMainLoop *loop = NULL;
static Adapter *default_adapter = NULL;
static Advertisement *advertisement = NULL;
static Application *app = NULL;
static Agent *agent = NULL;
static volatile sig_atomic_t cleanup_in_progress = 0;

static TKL_BLE_GAP_EVT_FUNC_CB  __gap_evt_cb  = NULL;
static TKL_BLE_GATT_EVT_FUNC_CB __gatt_evt_cb = NULL;

////////////////////////////////////////////////
//管理 characteristic 到 uuid(16bits) 的映射关系
////////////////////////////////////////////////
// 这是一个简单的结构体，用于存储 UUID 字符串和对应的序号
typedef struct {
    const char* uuid_str;
    uint16_t    index;
} ServCharMapEntry;

// 全局静态变量，用于存储映射表和当前分配的序号
// 假设最大特征值数量不会超过某个预设值，例如 32
#define MAX_SERVS 10
#define MAX_CHARS 32
static ServCharMapEntry serv_map[MAX_SERVS];
static ServCharMapEntry char_map[MAX_CHARS];
static uint16_t serv_count = 0;
static uint16_t char_count = 0;

static uint16_t _add_serv_info(const char *serv_uuid_str) {
    // 将 UUID 和序号存入映射表
    uint16_t service_index = 0xFFFF;
    if (serv_count < MAX_SERVS) {
        service_index = serv_count;

        serv_map[serv_count].uuid_str = g_strdup(serv_uuid_str); // 复制字符串，因为稍后会被释放
        serv_map[serv_count].index = service_index; 
        log_debug("SET GATT", "serv uuid index: %d", serv_count);
        serv_count++;
    } else {
        log_error("SET GATT", "Max service count exceeded!");
    }
    return service_index;
}
static uint16_t _add_char_info(const char *char_uuid_str) {
    // 将 UUID 和序号存入映射表
    uint16_t characteristic_index = 0xFFFF;
    if (char_count < MAX_CHARS) {
        characteristic_index = char_count+serv_count*10;   // 两位数，十位表示服务索引，个位表示特征索引

        char_map[char_count].uuid_str = g_strdup(char_uuid_str); // 复制字符串，因为 char_uuid_str 稍后会被释放
        char_map[char_count].index = characteristic_index; 
        log_debug("SET GATT", "char uuid index: %d", char_count);
        char_count++;
    } else {
        log_error("SET GATT", "Max characteristic count exceeded!");
    }
    return characteristic_index;
}
static uint16_t _get_serv_index_by_uuid(const char *serv_uuid) {
    uint16_t service_index = 0xFFFF; // 使用一个无效值作为默认值

    // 遍历映射表以查找 UUID
    for (int i = 0; i < serv_count; i++) {
        if (g_strcmp0(serv_map[i].uuid_str, serv_uuid) == 0) {
            service_index = serv_map[i].index;
            break;
        }
    }

    return service_index;
}
static uint16_t _get_char_index_by_uuid(const char *char_uuid) {
    uint16_t characteristic_index = 0xFFFF; // 使用一个无效值作为默认值

    // 遍历映射表以查找 UUID
    for (int i = 0; i < char_count; i++) {
        if (g_strcmp0(char_map[i].uuid_str, char_uuid) == 0) {
            characteristic_index = char_map[i].index;
            break;
        }
    }

    return characteristic_index;
}
static char * _get_serv_uuid_by_char_index(uint16_t char_index) {
    uint16_t serv_index = char_index / 10;
    for (int i = 0; i < serv_count; i++) {
        if (serv_map[i].index == serv_index) {
            return serv_map[i].uuid_str;
        }
    }
    return NULL;
}
static char * _get_char_uuid_by_char_index(uint16_t char_index) {
    for (int i = 0; i < char_count; i++) {
        if (char_map[i].index == char_index) {
            return char_map[i].uuid_str;
        }
    }
    return NULL;   
}
////////////////////////////////////////////////

static void on_powered_state_changed(Adapter *adapter, gboolean state) {
    log_debug(TAG, "powered '%s' (%s)", state ? "on" : "off", binc_adapter_get_path(adapter));
}

#define BLE_CONN_HANDLE 0x0001
static Device *sg_device = NULL;
static void on_central_state_changed(Adapter *adapter, Device *device) {
    char *deviceToString = binc_device_to_string(device);
    log_debug(TAG, deviceToString);
    g_free(deviceToString);

    log_debug(TAG, "remote central %s is %s", binc_device_get_address(device), binc_device_get_connection_state_name(device));
    ConnectionState state = binc_device_get_connection_state(device);
    if (state == BINC_CONNECTED) {
        sg_device = device;
        binc_adapter_stop_advertising(adapter, advertisement);
    } else if (state == BINC_DISCONNECTED){
        sg_device = NULL;
        // binc_adapter_start_advertising(adapter, advertisement);
        // 连接之后的停止广播由底层控制；断开后的开启广播由上层控制
    }
    

    // callback
    TKL_BLE_GAP_PARAMS_EVT_T event;
    memset(&event, 0, SIZEOF(TKL_BLE_GAP_PARAMS_EVT_T));
     
    event.result = 0;
    if (state == BINC_CONNECTED) {
        event.type = TKL_BLE_GAP_EVT_CONNECT;
    } else if (state == BINC_DISCONNECTED){
        event.type = TKL_BLE_GAP_EVT_DISCONNECT;
    }
    event.conn_handle = BLE_CONN_HANDLE;
    event.gap_event.connect.role = TKL_BLE_ROLE_SERVER;

    if (__gap_evt_cb) {
        __gap_evt_cb(&event);
    }
}

// This function is called when a read is done
// Use this to set the characteristic value if it is not set or to reject the read request
static const char *on_local_char_read(const Application *application, const char *address, const char *service_uuid,
                        const char *char_uuid) {
    const guint8 bytes[] = {0x06, 0x6f, 0x01, 0x00, 0xff, 0xe6, 0x07, 0x03, 0x03, 0x10, 0x04, 0x00, 0x01};
    GByteArray *byteArray = g_byte_array_sized_new(sizeof(bytes));
    g_byte_array_append(byteArray, bytes, sizeof(bytes));
    binc_application_set_char_value(application, service_uuid, char_uuid, byteArray);
    g_byte_array_free(byteArray, TRUE);

    TKL_BLE_GATT_PARAMS_EVT_T gatt_evt;
    memset(&gatt_evt, 0, sizeof(TKL_BLE_GATT_PARAMS_EVT_T));
    gatt_evt.type = TKL_BLE_GATT_EVT_READ_CHAR_VALUE;   
    gatt_evt.conn_handle = 0;
    gatt_evt.result = 0;
    gatt_evt.gatt_event.char_read.char_handle = _get_char_index_by_uuid(char_uuid);
    gatt_evt.gatt_event.char_read.offset = 0;

    if (__gatt_evt_cb != NULL) {
        __gatt_evt_cb(&gatt_evt);
    }

    return NULL;
}

// This function should be used to validate or reject a write request
static const char *on_local_char_write(const Application *application, const char *address, const char *service_uuid,
                          const char *char_uuid, GByteArray *byteArray) {
    GString *result = g_byte_array_as_hex(byteArray);
    log_debug(TAG, "write request characteristic <%s> with value <%s>", char_uuid, result->str);
    g_string_free(result, TRUE);

    TKL_BLE_GATT_PARAMS_EVT_T gatt_evt;
    memset(&gatt_evt, 0, sizeof(TKL_BLE_GATT_PARAMS_EVT_T));
    gatt_evt.type = TKL_BLE_GATT_EVT_WRITE_REQ;   
    gatt_evt.conn_handle = 0;
    gatt_evt.result = 0;
    gatt_evt.gatt_event.write_report.char_handle = _get_char_index_by_uuid(char_uuid);
    gatt_evt.gatt_event.write_report.report.p_data = byteArray->data;
    gatt_evt.gatt_event.write_report.report.length = byteArray->len;

    if (__gatt_evt_cb != NULL) {
        __gatt_evt_cb(&gatt_evt);
    }

    return NULL;
}

// This function is called after a write request was validates and the characteristic value was set
static void on_local_char_updated(const Application *application, const char *service_uuid,
                           const char *char_uuid, GByteArray *byteArray) {
    GString *result = g_byte_array_as_hex(byteArray);
    log_debug(TAG, "characteristic <%s> updated to <%s>", char_uuid, result->str);
    g_string_free(result, TRUE);
}

static void on_local_char_start_notify(const Application *application, const char *service_uuid, const char *char_uuid) {
    log_debug(TAG, "on start notify");
}

static void on_local_char_stop_notify(const Application *application, const char *service_uuid, const char *char_uuid) {
    log_debug(TAG, "on stop notify");
}

static gboolean on_request_authorization(Device *device) {
    log_debug(TAG, "requesting authorization for '%s", binc_device_get_name(device));
    return TRUE;
}

static guint32 on_request_passkey(Device *device) {
    guint32 pass = 000000;
    log_debug(TAG, "requesting passkey for '%s", binc_device_get_name(device));
    log_debug(TAG, "Enter 6 digit pin code: ");
    int result = fscanf(stdin, "%d", &pass);
    if (result != 1) {
        log_debug(TAG, "didn't read a pin code");
    }
    return pass;
}

static gboolean callback(gpointer data) {
    if (agent != NULL) {
        binc_agent_free(agent);
        agent = NULL;
    }

    if (default_adapter != NULL) {
        if (app != NULL) {
            binc_adapter_unregister_application(default_adapter, app);
            binc_application_free(app);
            app = NULL;
        }

        if (advertisement != NULL) {
            binc_adapter_stop_advertising(default_adapter, advertisement);
            binc_advertisement_free(advertisement);
            advertisement = NULL;
        }

        binc_adapter_free(default_adapter);
        default_adapter = NULL;
    } else {
        if (app != NULL) {
            binc_application_free(app);
            app = NULL;
        }
        if (advertisement != NULL) {
            binc_advertisement_free(advertisement);
            advertisement = NULL;
        }
    }

    if (data != NULL) {
        g_main_loop_quit((GMainLoop *) data);
    }
    return FALSE;
}

static void cleanup_handler(int signo) {
    if (signo == SIGINT) {
        if (cleanup_in_progress) {
            _exit(0);
        }
        cleanup_in_progress = 1;
        log_error(TAG, "received SIGINT");
        callback(loop);
        _exit(0);
    }
}

////////////////////////////////////////////////////////////////////////////////////
// 2. GAP 相关
// 简化的 AD 数据解析器，用于从原始字节流中提取本地名称、服务 UUID 等信息
static void parse_and_update_adv_data(Advertisement *advertisement, TKL_BLE_DATA_T const *p_data) {
    if (!p_data || !p_data->p_data || p_data->length == 0) {
        log_debug("AD_PARSER", "p_data is NULL or empty.");
        return;
    }
    uint8_t *cursor = p_data->p_data;
    uint16_t remaining_len = p_data->length;

    GPtrArray *service_uuids_list = g_ptr_array_new();
    char *local_name = NULL;

    // 临时存储服务数据，以便在设置服务列表之后再进行设置
    GHashTable *temp_service_data = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_byte_array_unref);

    while (remaining_len > 0) {
        uint8_t ad_len = cursor[0];
        if (ad_len == 0 || ad_len >= remaining_len) {
            break;
        }

        uint8_t ad_type = cursor[1];
        uint8_t *ad_payload = &cursor[2];
        uint16_t payload_len = ad_len - 1;

        switch (ad_type) {
            case 0x01: // Flags
                if (payload_len >= 1) {
                    guint8 flags = ad_payload[0];
                    // 检查 LE General Discoverable Mode (第 1 位)
                    if (flags & 0x02) {
                        binc_advertisement_set_general_discoverable(advertisement, TRUE);
                    } else {
                        binc_advertisement_set_general_discoverable(advertisement, FALSE);
                    }
                }
                break;
            case 0x02: // Incomplete List of 16-bit Service Class UUIDs
            case 0x03: // Complete List of 16-bit Service Class UUIDs
                for (int i = 0; i < payload_len; i += 2) {
                    char uuid_str[40];
                    snprintf(uuid_str, sizeof(uuid_str), "0000%04x-0000-1000-8000-00805f9b34fb", *(guint16 *)&ad_payload[i]);
                    g_ptr_array_add(service_uuids_list, g_strdup(uuid_str));
                }
                break;
            case 0x08: // Shortened Local Name
            case 0x09: // Complete Local Name
                // 仅取第一个发现的本地名称
                if (!local_name) {
                    local_name = g_strndup((const char *)ad_payload, payload_len);
                }
                break;
            case 0xFF: { // Manufacturer Specific Data
                if (payload_len >= 2) {
                    guint16 manufacturer_id = *(guint16 *)ad_payload;
                    GByteArray *data_array = g_byte_array_new();
                    g_byte_array_append(data_array, &ad_payload[2], payload_len - 2);
                    binc_advertisement_set_manufacturer_data(advertisement, manufacturer_id, data_array);
                    g_byte_array_unref(data_array);
                }
                break;
            }
            // 这里可以添加更多 AD 类型的解析，例如服务数据 (0x16)
            case 0x16: { // Service Data - 16-bit UUID
                 if (payload_len >= 2) {
                    guint16 service_uuid_val = *(guint16 *)ad_payload;
                    char service_uuid_str[40];
                    snprintf(service_uuid_str, sizeof(service_uuid_str), "0000%04x-0000-1000-8000-00805f9b34fb", service_uuid_val);
                    GByteArray *data_array = g_byte_array_new();
                    g_byte_array_append(data_array, &ad_payload[2], payload_len - 2);
   
                    g_hash_table_insert(temp_service_data, g_strdup(service_uuid_str), data_array);
                 }
                 break;
            }
            // 例如键盘、鼠标、耳机等，通过 2 字节的数据来表示具体的外观编码
            case 0x19: { // 0x19 Appearance（外观） 
                if (payload_len == 2) {                  
                    guint16 appearance = *(guint16 *)ad_payload;
                    binc_advertisement_set_appearance(advertisement, appearance);
                }
            }
            default:
                break;
        }

        cursor += ad_len + 1;
        remaining_len -= (ad_len + 1);
    }

    // 更新本地名称
    if (local_name) {
        log_debug("UPDATE ADV LOCAL NAME", "%s", local_name);
        binc_advertisement_set_local_name(advertisement, local_name);
        g_free(local_name);
    }

    // 更新服务 UUIDs
    if (service_uuids_list->len > 0) {
        log_debug("UPDATE ADV UUID", "num=%d, %s", service_uuids_list->len, g_strdup(g_ptr_array_index(service_uuids_list,0)));
        binc_advertisement_set_services(advertisement, service_uuids_list);
    }

    // 设置服务数据，必须在设置服务列表之后
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, temp_service_data);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        log_debug("UPDATE ADV SERVICE DATA", "0x16-> %s", key);
        log_debug("UPDATE ADV SERVICE DATA", "data_array[%d]",((GByteArray*)value)->len);
        binc_advertisement_set_service_data(advertisement, (const char*)key, (const GByteArray*)value);
    }

    g_ptr_array_free(service_uuids_list, TRUE);
    g_hash_table_destroy(temp_service_data);
}

void bluez_inc_update_adv(TKL_BLE_DATA_T const *p_adv, TKL_BLE_DATA_T const *p_scan_rsp) {
    log_debug("UPDATE ADV", "begin");

    // 尝试获取当前正在运行的广告对象
    advertisement = binc_adapter_get_advertisement(default_adapter);
    log_debug("UPDATE ADV", "get advertisement=%x", advertisement);

    if (advertisement == NULL) {
        log_debug("UPDATE ADV", "Advertisement object is not created. Creating one.");
        advertisement = binc_advertisement_create();
        if (advertisement == NULL) {
            log_debug("UPDATE ADV", "Failed to create advertisement object.");
            return;
        }
    }
   
    
    // 如果广告正在运行，先停止它
    //binc_adapter_stop_advertising(default_adapter, advertisement);

    // 解析并更新广播数据
    log_debug("UPDATE ADV", "Processing advertising data.");
    parse_and_update_adv_data(advertisement, p_adv);

    // 解析并更新扫描响应数据
    log_debug("UPDATE ADV", "Processing scan response data.");
    parse_and_update_adv_data(advertisement, p_scan_rsp);
    
    binc_advertisement_set_interval(advertisement, 20, 100);
    // binc_advertisement_set_tx_power(advertisement, 5);//0x0A flag
    //binc_advertisement_set_general_discoverable(advertisement, TRUE);
    // 重新启动广播以应用新的配置
    binc_adapter_start_advertising(default_adapter, advertisement);

    log_debug("UPDATE ADV", "end");
}

void bluez_inc_start_adv(void){
    log_debug("START ADV", "begin");

    // 尝试获取当前正在运行的广告对象
    advertisement = binc_adapter_get_advertisement(default_adapter);
    log_debug("UPDATE ADV", "get advertisement=%x", advertisement);

    if (advertisement == NULL) {
        log_debug("UPDATE ADV", "Advertisement object is not created. Creating one.");
        advertisement = binc_advertisement_create();
        if (advertisement == NULL) {
            log_debug("UPDATE ADV", "Failed to create advertisement object.");
            return;
        }
    }
   
    
    // 如果广告正在运行，先停止它
    //binc_adapter_stop_advertising(default_adapter, advertisement);
    // 重新启动广播以应用新的配置
    binc_adapter_start_advertising(default_adapter, advertisement);
    log_debug("UPDATE ADV", "end");   
}

void bluez_inc_disconnect(void){
    log_debug("DISCONNECT", "begin");
    if (sg_device != NULL) {
        binc_device_disconnect(sg_device);
    }
}

////////////////////////////////////////////////////////////////////////////////////
// 3. GATT 相关
// 辅助函数：将 TKL_BLE_UUID_T 转换为 C 字符串（使用动态内存分配，更安全）
char *uuid_to_string(const TKL_BLE_UUID_T* p_uuid) {
    // 动态分配内存，调用者有责任释放它
    char* uuid_str = (char*)malloc(40 * sizeof(char));
    if (!uuid_str) {
        return NULL;
    }
    memset(uuid_str, 0, 40);

    switch (p_uuid->uuid_type) {
        case TKL_BLE_UUID_TYPE_16:
            snprintf(uuid_str, 40, "0000%04x-0000-1000-8000-00805f9b34fb", p_uuid->uuid.uuid16);
            break;
        case TKL_BLE_UUID_TYPE_32:
            snprintf(uuid_str, 40, "0000%08x-0000-1000-8000-00805f9b34fb", p_uuid->uuid.uuid32);
            break;
        case TKL_BLE_UUID_TYPE_128:
            snprintf(uuid_str, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     p_uuid->uuid.uuid128[15], p_uuid->uuid.uuid128[14], p_uuid->uuid.uuid128[13], p_uuid->uuid.uuid128[12],
                     p_uuid->uuid.uuid128[11], p_uuid->uuid.uuid128[10],
                     p_uuid->uuid.uuid128[9], p_uuid->uuid.uuid128[8],
                     p_uuid->uuid.uuid128[7], p_uuid->uuid.uuid128[6],
                     p_uuid->uuid.uuid128[5], p_uuid->uuid.uuid128[4], p_uuid->uuid.uuid128[3], p_uuid->uuid.uuid128[2], p_uuid->uuid.uuid128[1], p_uuid->uuid.uuid128[0]);
            break;
        default:
            free(uuid_str);
            return NULL;
    }
    return uuid_str;
}

// 根据 p_service 内容实现 GATT 设置
void bluez_inc_add_gatt(TKL_BLE_GATTS_PARAMS_T *p_service) {
    log_debug("SET GATT", "begin");

    // 创建应用实例
    app = binc_create_application(default_adapter);
    if (!app) {
        log_debug("SET GATT", "Failed to create application");
        return;
    }

    // 遍历每一个服务
    for (int i = 0; i < p_service->svc_num; i++) {
        TKL_BLE_SERVICE_PARAMS_T *p_svc = &(p_service->p_service[i]);

        // 将服务的 UUID 转换为字符串
        char *service_uuid_str = uuid_to_string(&p_svc->svc_uuid);
        if (!service_uuid_str) {
            log_debug("SET GATT", "Invalid UUID type for service %d", i);
            continue;
        }

        // 添加服务
        printf("service_uuid_str:%s\n", service_uuid_str);
        if (binc_application_add_service(app, service_uuid_str) != 0) {
            log_debug("SET GATT", "Failed to add service with UUID: %s", service_uuid_str);
            free(service_uuid_str); 
            continue;
        }

        // 遍历当前服务下的每一个特征值
        for (int j = 0; j < p_svc->char_num; j++) {
            TKL_BLE_CHAR_PARAMS_T *p_char = &(p_svc->p_char[j]);

            // 将特征值的 UUID 转换为字符串
            char *char_uuid_str = uuid_to_string(&p_char->char_uuid);
            if (!char_uuid_str) {
                log_debug("SET GATT", "Invalid UUID type for characteristic %d of service %s", j, service_uuid_str);
                continue;
            }

            // 添加特征值。我们假设 p_char->property 字段的位定义与 binc 库兼容。
            log_debug("SET GATT", "add char_uuid_str:%s -> service_uuid_str:%s, property:%x", char_uuid_str, service_uuid_str, p_char->property);
            if (binc_application_add_characteristic(app, service_uuid_str, char_uuid_str, p_char->property) != 0) {
                log_debug("SET GATT", "Failed to add characteristic with UUID: %s", char_uuid_str);
                free(char_uuid_str);
                continue;
            }

            // 将 UUID 和序号存入映射表
            // 同时将 index 作为 handle 给上层（通过 p_svc 指针）
            p_svc->p_char[j].handle = _add_char_info(char_uuid_str);

            free(char_uuid_str);
        }

        // 将 SERVICE 和序号存入映射表
        _add_serv_info(service_uuid_str);
        free(service_uuid_str); 
    }

    // 设置回调函数
    binc_application_set_char_read_cb(app, &on_local_char_read);
    binc_application_set_char_write_cb(app, &on_local_char_write);
    binc_application_set_char_start_notify_cb(app, &on_local_char_start_notify);
    binc_application_set_char_stop_notify_cb(app, &on_local_char_stop_notify);
    binc_application_set_char_updated_cb(app, &on_local_char_updated);

    // 注册应用到适配器
    binc_adapter_register_application(default_adapter, app);

    log_debug("SET GATT", "end");
}

int bluez_inc_gatt_value_notify(uint16_t conn_handle, uint16_t char_handle, uint8_t *p_data, uint16_t length) {
    char * serv_uuid_str = _get_serv_uuid_by_char_index(char_handle);
    char * char_uuid_str = _get_char_uuid_by_char_index(char_handle);
    if (serv_uuid_str && char_uuid_str) {
        log_debug("GATT_NOTIFY", "serv_uuid=%s, char_uuid=%s", serv_uuid_str, char_uuid_str);
        GByteArray *byteArray = g_byte_array_sized_new(length);
        g_byte_array_append(byteArray, p_data, length);
        binc_application_notify(app, serv_uuid_str, char_uuid_str, byteArray);
        g_byte_array_free(byteArray, TRUE);   
        return 0;//OPRT_OK
    }
    return -2;//OPRT_INVALID_PARM
}

////////////////////////////////////////////////////////////////////////////////////
// 1. 初始化相关
static GDBusConnection *dbusConnection = NULL;
static pthread_t my_thread_id;
void* _thread_function(void* arg) {
    // Start the mainloop
    g_main_loop_run(loop);

    // Clean up mainloop
    g_main_loop_unref(loop);

    // Disconnect from DBus
    g_dbus_connection_close_sync(dbusConnection, NULL, NULL);
    g_object_unref(dbusConnection);
    return NULL;
}

static bool is_bluez_inc_init = false;
void bluez_inc_init(TKL_BLE_GAP_EVT_FUNC_CB gap_evt_cb, TKL_BLE_GATT_EVT_FUNC_CB gatt_evt_cb){
    if (is_bluez_inc_init) return;
    is_bluez_inc_init = true;

    __gap_evt_cb = gap_evt_cb;
    __gatt_evt_cb = gatt_evt_cb;

    // Get a DBus connection
    dbusConnection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

    // Setup handler for CTRL+C
    if (signal(SIGINT, cleanup_handler) == SIG_ERR)
        log_error(TAG, "can't catch SIGINT");

    // Setup mainloop
    loop = g_main_loop_new(NULL, FALSE);

    // Get the default default_adapter
    default_adapter = binc_adapter_get_default(dbusConnection);

    if (default_adapter != NULL) {
        log_debug(TAG, "using default_adapter '%s'", binc_adapter_get_path(default_adapter));

        // Make sure the adapter is on
        // binc_adapter_power_off(default_adapter);
        binc_adapter_set_powered_state_cb(default_adapter, &on_powered_state_changed);
        if (!binc_adapter_get_powered_state(default_adapter)) {
            binc_adapter_power_on(default_adapter);
        }

        // Register an agent and set callbacks
        // agent = binc_agent_create(default_adapter, "/org/bluez/BincAgent", KEYBOARD_DISPLAY);
        // binc_agent_set_request_authorization_cb(agent, &on_request_authorization);
        // binc_agent_set_request_passkey_cb(agent, &on_request_passkey);

        // Setup remote central connection state callback
        binc_adapter_set_remote_central_cb(default_adapter, &on_central_state_changed);

        log_debug(TAG, "Stack Init OK");
    } else {
        log_debug(TAG, "No default_adapter found");
    }

    // Bail out after some time
    // 600s 后回收资源
    // g_timeout_add_seconds(600, callback, loop);

    // Using thread to run mainloop
    pthread_create(&my_thread_id, NULL, _thread_function, NULL);
}

void bluez_inc_deinit(void){
    
}
