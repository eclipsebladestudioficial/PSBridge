#ifndef PSBRIDGE_H
#define PSBRIDGE_H

#include <tamtypes.h>

typedef struct {
    void *memory;
    u32 base;
    u32 memory_size;
    u32 file_size;
    u32 gp;
} GuestInfo;

typedef struct {
    u32 nid;
    u32 library;
    u32 stub;
    const void *wrapper;
} PspImport;

extern const GuestInfo guest_info;
extern const u32 psp_import_count;

int psbridge_start(void);
int psbridge_call_guest(void *entry, void *argument);
int psbridge_call_callback(void *entry, u32 count, u32 argument, u32 common);

void *psbridge_game_pointer(const void *pointer);
int psbridge_map_game_memory(void);
void *psbridge_video_memory(void);
int psbridge_controller_read(void *output, int count, u32 nid);
int psbridge_controller_sampling(int mode);
int psbridge_controller_get_sampling(void *output);
int psbridge_utility(u32 nid, void *argument);
u64 psbridge_display(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3);
u64 psbridge_video_mpeg(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5);
int psbridge_graphics_start(void);
void psbridge_graphics_present(void);
void psbridge_video_render_frame(u16 *pixels);
void psbridge_video_log_frame(u32 frame_count);
u64 psbridge_ge(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3);
u64 psbridge_audio(u32 library, u32 nid, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4);

int psbridge_alloc_partition(int partition, const char *name, int type,u32 size,void *address);

int psbridge_volatile_lock(void *address, void *size);
int psbridge_volatile_unlock(void);

void *psbridge_block_address(int uid);
int psbridge_free_partition(int uid);

int psbridge_create_fpl(const char *name, int partition, u32 attributes, u32 block_size, u32 block_count, void *options);

int psbridge_allocate_fpl(int uid, void **output);
int psbridge_delete_fpl(int uid);
int psbridge_create_vpl(const char *name, int partition, u32 attributes, u32 size, void *options);
int psbridge_allocate_vpl(int uid, u32 size, void *output, int try_only);
int psbridge_free_vpl(int uid, void *address);
int psbridge_delete_vpl(int uid);
int psbridge_refer_vpl_status(int uid, void *output);
int psbridge_create_semaphore(const char *name, u32 attributes, int initial, int maximum);
int psbridge_delete_semaphore(int uid);
int psbridge_signal_semaphore(int uid, int count);
int psbridge_wait_semaphore(int uid, int count, void *timeout);
int psbridge_wakeup_guest_thread(int uid);
int psbridge_delete_guest_thread(int uid);
int psbridge_wait_guest_thread_end(int uid, void *timeout, int use_callbacks);
int psbridge_create_mutex(const char *name, u32 attributes, int initial_count, void *options);
int psbridge_refer_mutex_status(int uid, void *output);
int psbridge_lock_mutex(int uid, int count, void *timeout, int try_only);
int psbridge_unlock_mutex(int uid, int count);
int psbridge_delete_mutex(int uid);
int psbridge_change_thread_priority(int uid, int priority);
void psbridge_exit_guest_thread(int status);

int psbridge_create_thread(const char *name, void *entry, int priority, int stack_size, u32 attributes, void *options);

int psbridge_start_thread(int uid, u32 arg_size, void *arg);
int psbridge_psp_thread_uid(void);
void *psbridge_thread_kernel_work(void);
int psbridge_create_lwmutex(void *workarea, u32 attributes, int initial_count);
int psbridge_lock_lwmutex_ex(void *workarea, int count, void *timeout, int use_callbacks, int try_only);
int psbridge_delete_lwmutex(void *workarea);
int psbridge_lock_lwmutex(void *workarea, int count);
int psbridge_unlock_lwmutex(void *workarea, int count);

int psbridge_file_chdir(const char *path);
int psbridge_file_dopen(const char *path);
int psbridge_file_dread(int uid, void *output);
int psbridge_file_dclose(int uid);
int psbridge_file_open(const char *path, int flags, int mode);
int psbridge_file_close(int descriptor);
int psbridge_file_open_async(const char *path, int flags, int mode);
int psbridge_file_async_wait(int descriptor, void *output, int polling);
int psbridge_file_read_async(int descriptor, void *buffer, u32 size);
int psbridge_file_write_async(int descriptor, const void *buffer, u32 size);
int psbridge_file_close_async(int descriptor);

int psbridge_file_read(int descriptor, void *buffer, u32 size);
int psbridge_file_write(int descriptor, const void *buffer, u32 size);
s64 psbridge_file_seek(int descriptor, u32 low, u32 high, int whence);
int psbridge_file_stat(const char *path, void *output);
int psbridge_file_remove(const char *path);
int psbridge_file_rename(const char *old_path, const char *new_path);
int psbridge_file_ioctl(int descriptor, u32 command, void *input, u32 input_size, void *output, u32 output_size);
int psbridge_umd_present(void);
u32 psbridge_umd_state(void);
int psbridge_umd_activate(int mode, const char *name);
int psbridge_umd_deactivate(int mode, const char *name);
int psbridge_umd_wait(u32 state);
int psbridge_umd_wait_cb(u32 state, u32 timeout);
int psbridge_umd_register_callback(int uid);
int psbridge_umd_unregister_callback(int uid);


/*
Vai tomar no cu se essa porra não funcionar vou mandar todo mundo tomar 
no cu porque puta que pariu

*/

int psbridge_create_mailbox(const char *name, u32 attributes, void *options);
int psbridge_send_mailbox(int uid, void *message);
int psbridge_receive_mailbox(int uid, void *output, void *timeout);
int psbridge_delete_mailbox(int uid);

u32 psbridge_suspend_intr(void);
void psbridge_resume_intr(u32 flags);
int psbridge_sync_init(void);
int psbridge_create_event(u32 attributes, u32 initial);
int psbridge_delete_event(int uid);
int psbridge_set_event(int uid, u32 bits);
int psbridge_clear_event(int uid, u32 mask);
int psbridge_wait_event(int uid, u32 bits, u32 mode, void *output, void *timeout, int use_callbacks, int polling);
int psbridge_create_callback(u32 function, u32 common);
int psbridge_delete_callback(int uid);
int psbridge_notify_callback(int uid, u32 argument);
int psbridge_cancel_callback(int uid);
int psbridge_callback_count(int uid);
int psbridge_check_callbacks(void);
int psbridge_delay_thread(u32 delay, int use_callbacks);


#endif
