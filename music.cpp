#include <stdio.h>
#include <stdlib.h>
#include <conio.h> 
#include <dos.h> 
#include <string.h>
#include <stdarg.h>  
#include <pc.h>      
#include <dpmi.h>    
#include <go32.h>    
#include <sys/farptr.h>
#include <dir.h>     
#include <unistd.h>
#include <time.h>
#include <ctype.h>

#define CONF_486_DIVISOR   1      
#define CONF_486_CHANNELS  2      
#define VISUALIZER_GLOBAL_BOOST 3 

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define DSP_BASE   0x220
#define DSP_RESET  (DSP_BASE + 0x6)
#define DSP_READ   (DSP_BASE + 0xA)
#define DSP_WRITE  (DSP_BASE + 0xC)
#define DSP_STATUS (DSP_BASE + 0xE)
#define DSP_16_ACK (DSP_BASE + 0xF) 

#define MIXER_ADDR 0x224
#define MIXER_DATA 0x225
#define SB_IRQ     7
#define IRQ_VECTOR (SB_IRQ + 8) 

#define DMA8_MASK   0x0A
#define DMA8_MODE   0x0B
#define DMA8_CLEAR  0x0C
#define DMA8_ADDR   0x02
#define DMA8_COUNT  0x03
#define DMA8_PAGE   0x83

#define DMA16_MASK  0xD4
#define DMA16_MODE  0xD6
#define DMA16_CLEAR 0xD8
#define DMA16_ADDR  0xC4
#define DMA16_COUNT 0xC6
#define DMA16_PAGE  0x8B

unsigned long physical_addr = 0;

#define MOUSE_INT 0x33
#define INIT_MOUSE 0x00
#define SHOW_MOUSE 0x01
#define HIDE_MOUSE 0x02
#define GET_MOUSE_STATUS 0x03
#define CURSOR_CHAR 0x0D
int mouse_x = 0, mouse_y = 0, mouse_left = 0, mouse_right = 0;
int has_mouse = 0; 
int use_mouse = 1; 

_go32_dpmi_seginfo old_isr, new_isr, dos_buffer;

volatile int interrupt_count = 0; 
volatile int refill_request = 0; 

int current_sample_rate = 44100;
int current_channels = 2;
int master_volume = 100; 
int is_paused = 0;
int is_looping = 0;

volatile int frames_decoded = 0;
volatile int buffer_skips = 0;
int pcm_leftover_bytes = 0;

int global_is_pc_speaker = 0;
int config_is_pc_speaker = 0;
int global_out_sample_rate = 44100;
int global_out_channels = 2;
int global_is_486 = 0;
int config_is_486 = 0;
int active_bitdepth = 16;
volatile int active_buffer_size = 65536;
int has_active_file = 0;

char id3_title[31] = {0};
char id3_artist[31] = {0};
// --- NEW MEDIA INFO GLOBALS ---
char id3_album[31] = {0};
char id3_year[5] = {0};
char id3_label[31] = {0};

char id3_album_artist[31] = {0};
char id3_track[10] = {0};
char id3_genre[31] = {0};
char id3_composer[31] = {0};
char id3_copyright[31] = {0};
char id3_encoded_by[31] = {0};
char id3_bpm[10] = {0};
char id3_key[10] = {0};
char id3_remix[31] = {0};
char id3_subtitle[31] = {0}; // TIT3 frame for additional info like remix titles, movement names, or descriptive subtitles

int file_bitrate_kbps = 0;
int file_is_vbr = 0;
int info_scroll = 0;
// ------------------------------
int has_id3 = 0;

// --- PLAYBACK ENGINE STATE GLOBALS ---
int custom_sample_rate = 0;
int custom_channels = 0;
int custom_buffer_size = 0;
int is_sb16 = 0;
int dsp_major = 0;
int dsp_minor = 0;
int has_auto_init = 0; // Tracks if DSP is v2.01+ for continuous DMA
long start_off = 0;
long file_size = 0;

// --- NEW DISK STREAMING ENGINE GLOBALS ---
FILE* active_audio_file = NULL;
unsigned char stream_buffer[65536];
int stream_bytes = 0;
// -----------------------------------------

unsigned char* mp3_ram_cache = NULL;
long chunk_size = 65536;
long bytes_remaining = 0;
unsigned char* current_ram_ptr = NULL;
unsigned char* pcm_ram_cache = NULL;
long pcm_bytes_remaining = 0;
long global_wav_bytes = 0;
int ui_total_seconds = 0;
unsigned int resample_step = 0;
unsigned int resample_pos = 0;
unsigned long ui_total_samples_played = 0;
int target_sample_rate = 44100;
int target_channels = 2;

mp3dec_t mp3d;
mp3dec_frame_info_t info;
short pcm_temp[MINIMP3_MAX_SAMPLES_PER_FRAME];
short resampled_temp[MINIMP3_MAX_SAMPLES_PER_FRAME * 6]; 

// --- UI VIEWS & MENU STATE ---
int ui_view = 0; // 0 = Player, 1 = File Browser, 2 = Settings, 3 = Playlist, 4 = Media Info
int active_menu = 0; // 0=None, 1=File, 2=Audio, 3=Visual
int active_menu_hover = -1;
int setting_ansi_mode = 1;
int browser_needs_redraw = 1; 
int force_ui_redraw = 1; // Global UI cache invalidator
int settings_saved_flag = 0;

// --- PLAYLIST STATE GLOBALS ---
#define MAX_QUEUE 1000
char queue_paths[MAX_QUEUE][512];
char queue_displays[MAX_QUEUE][64];
int queue_count = 0;
int queue_current = -1;
int queue_selected = -1;
int queue_scroll = 0;

// --- SETTINGS STATE GLOBALS ---
int current_settings_tab = 1;
int setting_hover_effects = 1;
int setting_color = 1;
int setting_bg_color = 0;  
int setting_fg_color = 7;  
int setting_sel_color = 1; 
int setting_crit_color = 4;
int active_tab_element = -1;

const char* file_menu[] = { 
    "\xC3\xC4\xC4\xC4\xC4\xC4\xC4\xC1\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC1\xC2\xC4",
    "\xB3 Open File     \xB3",
    "\xB3 Open Playlist \xB3",
    "\xB3 Media Info    \xB3",
    "\xB3 Settings      \xB3",
    "\xC3\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xB4",
    "\xB3 Exit          \xB3",
    "\xC3\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xD9"
};

const char* audio_menu[] = { 
    "\xC5\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC1\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC1\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC2",
    "\xB3 Set Device - S. Blaster \xB3",
    "\xB3 Set Device - PC Speaker \xB3",
    "\xC3\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xB4",
    "\xB3 Volume Up               \xB3",
    "\xB3 Volume Down             \xB3",
    "\xC3\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xB4",
    "\xB3 Toggle Loop             \xB3",
    "\xB3 Audio Settings          \xB3",
    "\xC0\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xD9"
};

const char* visual_menu[] = { 
    "\xC5\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC1\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC2",
    "\xB3 VU Meter           \xB3",
    "\xB3 Frequency Bars     \xB3",
    "\xB3 Debug Info         \xB3",
    "\xC3\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xB4",
    "\xB3 Visual Settings    \xB3",
    "\xC0\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xD9"
};

// --- FILE BROWSER STATE ---
typedef struct {
    char name[256];
    int is_dir;
    long size;
} FileEntry;

FileEntry all_file_list[512]; 
int all_file_count = 0;

FileEntry file_list[512];
int file_count = 0;
int file_scroll = 0;
int file_selected = -1;

char current_dir[512] = "."; 
char browser_status_msg[64] = ""; 
char app_filename[512] = {0};
int browser_enqueue_only = 0;
int browser_save_mode = 0;

char path_history[20][512];
int path_history_count = 0;
int path_history_index = -1;

// Extensible Interactive Input States
char dialog_search_text[256] = "";
char dialog_address_text[512] = "."; // DEDICATED Address input buffer
char dialog_file_name[256] = "";

int active_input_field = 0; // 0=None, 2=Search, 3=Address, 4=FileName
int dialog_search_cursor = 0;     
int dialog_filename_cursor = 0;
int dialog_address_cursor = 0;

int visualizer_mode = 0; // 0=None, 1=VU Meter, 2=Frequency Bars, 3=Debug Info
// --- VISUALIZER SETTINGS GLOBALS ---
int vis_vu_peaks = 1;       // 0=Off, 1=On
int vis_vu_falloff = 1;     // 0=Slow, 1=Normal, 2=Fast
int vis_vu_c1 = 10;         // Light Green
int vis_vu_c2 = 14;         // Yellow
int vis_vu_c3 = 12;         // Light Red

int vis_bar_peaks = 1;      // 0=Off, 1=On
int vis_bar_falloff = 1;    // 0=Slow, 1=Normal, 2=Fast
int vis_bar_style = 0;      // 0=Solid (219), 1=Shaded (178/177/176)
int vis_bar_c1 = 10;        // Light Green
int vis_bar_c2 = 14;        // Yellow
int vis_bar_c3 = 12;        // Light Red

int vis_dbg_refresh = 0;    // 0=Real-time, 1=500ms, 2=1000ms
int vis_dbg_detail = 0;     // 0=Standard, 1=Advanced
int vis_fps_cap = 0;        // 0=Uncapped, 1=30 FPS, 2=15 FPS
// -----------------------------------
int vu_left_peak = 0, vu_right_peak = 0;
int next_vu_left_peak = 0, next_vu_right_peak = 0;  
int smoothed_vu_left = 0, smoothed_vu_right = 0;   

#define NUM_BANDS 10
int spectrum_peaks[NUM_BANDS] = {0};
int next_spectrum_peaks[NUM_BANDS] = {0};
int smoothed_spectrum[NUM_BANDS] = {0};

unsigned char pc_speaker_dma[65536]; 
volatile int dma_play_pos = 0;
volatile int pit_divisor = 108;
volatile int pc_speaker_overdrive = 200; 

void refill_stream() {
    if (!active_audio_file) return;
    int consumed = current_ram_ptr - stream_buffer;
    int remaining = stream_bytes - consumed;
    
    // If we have less than 16KB of data left in the buffer, pull more from the hard drive!
    if (remaining < 16384 && !feof(active_audio_file)) {
        if (remaining > 0) memmove(stream_buffer, current_ram_ptr, remaining); // Slide leftovers to the front
        int to_read = 65536 - remaining;
        int bytes_read = fread(stream_buffer + remaining, 1, to_read, active_audio_file);
        stream_bytes = remaining + bytes_read;
        current_ram_ptr = stream_buffer;
    }
}

// --- FORWARD DECLARATIONS ---
unsigned char make_color(unsigned char bg, unsigned char fg);
unsigned char get_bg_color();
unsigned char get_hl_color();
unsigned char get_cr_color();
unsigned char get_dim_color();
unsigned char get_accent_color();
void draw_player_btn(int x, int w, const char* icon, unsigned char border_col, unsigned char icon_col);
void print_theme_line(int y, const char* label, int color_val, int use_color, unsigned char bg, unsigned char hl, int mx, int my, int hover_on);
void draw_visualizer();
void draw_file_browser();
void draw_settings();
void draw_menu();
void draw_playlist();
void clear_inner_ui();
void init_ui(const char* filename);
void update_ui(int current_sec, int total_sec);
void load_dir(const char* path, int update_history);
void load_new_file_from_browser(const char* filepath);
void save_playlist_m3u(const char* filepath);
void load_playlist_m3u(const char* filepath, int append);
void execute_browser_ok(); 
void draw_media_info();


// --- SETTINGS LOAD/SAVE ---
void load_config() {
    FILE* f = fopen("config.txt", "r");
    if (f) {
        char line[128];
        int val;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "setting_hover_effects=%d", &val) == 1) setting_hover_effects = val;
            else if (sscanf(line, "setting_ansi_mode=%d", &val) == 1) setting_ansi_mode = val;
            else if (sscanf(line, "setting_color=%d", &val) == 1) setting_color = val;
            else if (sscanf(line, "setting_bg_color=%d", &val) == 1) {
                if (val == 15) val = 7; 
                setting_bg_color = val;
            }
            else if (sscanf(line, "setting_fg_color=%d", &val) == 1) {
                if (val == 15) val = 7; 
                setting_fg_color = val;
            }
            else if (sscanf(line, "setting_sel_color=%d", &val) == 1) setting_sel_color = val;
            else if (sscanf(line, "setting_crit_color=%d", &val) == 1) setting_crit_color = val;
            else if (sscanf(line, "custom_sample_rate=%d", &val) == 1) custom_sample_rate = val;
            else if (sscanf(line, "custom_channels=%d", &val) == 1) custom_channels = val;
            else if (sscanf(line, "custom_buffer_size=%d", &val) == 1) custom_buffer_size = val;
            else if (sscanf(line, "pc_speaker_overdrive=%d", &val) == 1) pc_speaker_overdrive = val;
            else if (sscanf(line, "global_is_pc_speaker=%d", &val) == 1) { 
                global_is_pc_speaker = val; 
                config_is_pc_speaker = val; 
            }
            else if (sscanf(line, "global_is_486=%d", &val) == 1) { 
                global_is_486 = val; 
                config_is_486 = val; 
            }

            // --- NEW: LOAD SAVED VISUALIZER ---
            else if (sscanf(line, "visualizer_mode=%d", &val) == 1) visualizer_mode = val;
            
            // --- NEW: VISUALIZER SETTINGS ---
            else if (sscanf(line, "vis_vu_peaks=%d", &val) == 1) vis_vu_peaks = val;
            else if (sscanf(line, "vis_vu_falloff=%d", &val) == 1) vis_vu_falloff = val;
            else if (sscanf(line, "vis_vu_c1=%d", &val) == 1) vis_vu_c1 = val;
            else if (sscanf(line, "vis_vu_c2=%d", &val) == 1) vis_vu_c2 = val;
            else if (sscanf(line, "vis_vu_c3=%d", &val) == 1) vis_vu_c3 = val;
            
            else if (sscanf(line, "vis_bar_peaks=%d", &val) == 1) vis_bar_peaks = val;
            else if (sscanf(line, "vis_bar_falloff=%d", &val) == 1) vis_bar_falloff = val;
            else if (sscanf(line, "vis_bar_style=%d", &val) == 1) vis_bar_style = val;
            else if (sscanf(line, "vis_bar_c1=%d", &val) == 1) vis_bar_c1 = val;
            else if (sscanf(line, "vis_bar_c2=%d", &val) == 1) vis_bar_c2 = val;
            else if (sscanf(line, "vis_bar_c3=%d", &val) == 1) vis_bar_c3 = val;
            
            else if (sscanf(line, "vis_dbg_refresh=%d", &val) == 1) vis_dbg_refresh = val;
            else if (sscanf(line, "vis_dbg_detail=%d", &val) == 1) vis_dbg_detail = val;
            else if (sscanf(line, "vis_fps_cap=%d", &val) == 1) vis_fps_cap = val;
        }
        fclose(f);
    }
}

void save_config() {
    FILE* f = fopen("config.txt", "w");
    if (f) {
        fprintf(f, "setting_hover_effects=%d\n", setting_hover_effects);
        fprintf(f, "setting_color=%d\n", setting_color);
        fprintf(f, "setting_ansi_mode=%d\n", setting_ansi_mode);
        fprintf(f, "setting_bg_color=%d\n", setting_bg_color);
        fprintf(f, "setting_fg_color=%d\n", setting_fg_color);
        fprintf(f, "setting_sel_color=%d\n", setting_sel_color);
        fprintf(f, "setting_crit_color=%d\n", setting_crit_color);
        fprintf(f, "custom_sample_rate=%d\n", custom_sample_rate);
        fprintf(f, "custom_channels=%d\n", custom_channels);
        fprintf(f, "custom_buffer_size=%d\n", custom_buffer_size);
        fprintf(f, "pc_speaker_overdrive=%d\n", pc_speaker_overdrive);
        fprintf(f, "global_is_pc_speaker=%d\n", config_is_pc_speaker);
        // --- SAVE THE STAGED CONFIG, NOT THE LIVE ENGINE STATE ---
        fprintf(f, "global_is_486=%d\n", config_is_486);

        // --- NEW: SAVE CURRENT VISUALIZER ---
        fprintf(f, "visualizer_mode=%d\n", visualizer_mode);
        
        // --- NEW: VISUALIZER SETTINGS ---
        fprintf(f, "vis_vu_peaks=%d\n", vis_vu_peaks);
        fprintf(f, "vis_vu_falloff=%d\n", vis_vu_falloff);
        fprintf(f, "vis_vu_c1=%d\n", vis_vu_c1);
        fprintf(f, "vis_vu_c2=%d\n", vis_vu_c2);
        fprintf(f, "vis_vu_c3=%d\n", vis_vu_c3);
        
        fprintf(f, "vis_bar_peaks=%d\n", vis_bar_peaks);
        fprintf(f, "vis_bar_falloff=%d\n", vis_bar_falloff);
        fprintf(f, "vis_bar_style=%d\n", vis_bar_style);
        fprintf(f, "vis_bar_c1=%d\n", vis_bar_c1);
        fprintf(f, "vis_bar_c2=%d\n", vis_bar_c2);
        fprintf(f, "vis_bar_c3=%d\n", vis_bar_c3);
        
        fprintf(f, "vis_dbg_refresh=%d\n", vis_dbg_refresh);
        fprintf(f, "vis_dbg_detail=%d\n", vis_dbg_detail);
        fprintf(f, "vis_fps_cap=%d\n", vis_fps_cap);
        
        fclose(f);
    }
}

// --- DIRECTORY PARSING & SEARCH ---

/* DJGPP Safe Case-Insensitive Substring Search */
int safe_strcasestr(const char* haystack, const char* needle) {
    int i, j;
    if (!needle || !*needle) return 1;
    if (!haystack) return 0;
    
    for (i = 0; haystack[i] != '\0'; i++) {
        j = 0;
        while (needle[j] != '\0') {
            unsigned char c1 = (unsigned char)haystack[i + j];
            unsigned char c2 = (unsigned char)needle[j];
            
            if (c1 == '\0') break; 
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            
            if (c1 != c2) break;
            j++;
        }
        if (needle[j] == '\0') return 1; 
    }
    return 0;
}

void filter_browser_files() {
    int i;
    file_count = 0;
    file_scroll = 0;
    file_selected = -1;

    for (i = 0; i < all_file_count; i++) {
        if (dialog_search_text[0] == '\0') {
            file_list[file_count++] = all_file_list[i];
        } else {
            if (safe_strcasestr(all_file_list[i].name, dialog_search_text)) {
                file_list[file_count++] = all_file_list[i];
            }
        }
    }
    browser_needs_redraw = 1;
}

int ends_with_ignore_case(const char* str, const char* suffix) {
    if (!str || !suffix) return 0;
    size_t len_str = strlen(str);
    size_t len_suffix = strlen(suffix);
    if (len_suffix > len_str) return 0;
    const char* a = str + len_str - len_suffix;
    const char* b = suffix;
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return 1;
}

void load_dir(const char* path, int update_history) {
    struct ffblk ff;
    char search_path[512];
    
    // Auto-sync current_dir if a new path string was passed
    if (path != current_dir) {
        strncpy(current_dir, path, 511);
        current_dir[511] = '\0';
    }
    
    int len = strlen(current_dir);
    if (len > 0 && current_dir[len-1] != '/' && current_dir[len-1] != '\\') {
        if (len < 510) strcat(current_dir, "\\");
    }
    
    // History Tracking logic
    if (update_history) {
        if (path_history_index < path_history_count - 1) {
            path_history_count = path_history_index + 1;
        }
        if (path_history_count == 0 || stricmp(path_history[path_history_count - 1], current_dir) != 0) {
            if (path_history_count < 20) {
                strcpy(path_history[path_history_count++], current_dir);
                path_history_index++;
            } else {
                for (int i = 1; i < 20; i++) strcpy(path_history[i-1], path_history[i]);
                strcpy(path_history[19], current_dir);
            }
        }
    }
    
    snprintf(search_path, 512, "%s*.*", current_dir);
    all_file_count = 0; 
    
    int done = findfirst(search_path, &ff, FA_DIREC);
    while (!done) {
        if (all_file_count >= 512) break;
        
        if (ff.ff_name[0] == '.' && strcmp(ff.ff_name, "..") != 0) {
            done = findnext(&ff);
            continue;
        }
        
        int is_dir = (ff.ff_attrib & FA_DIREC) != 0;

        if (!is_dir) {
            if (!ends_with_ignore_case(ff.ff_name, ".mp3") && 
                !ends_with_ignore_case(ff.ff_name, ".wav") &&
                !ends_with_ignore_case(ff.ff_name, ".m3u") && 
                !ends_with_ignore_case(ff.ff_name, ".m3u8")) {
                done = findnext(&ff);
                continue;
            }
        }

        strcpy(all_file_list[all_file_count].name, ff.ff_name);
        all_file_list[all_file_count].is_dir = is_dir;
        all_file_list[all_file_count].size = ff.ff_fsize; 
        all_file_count++;
        
        done = findnext(&ff);
    }
    
    // Auto-clear UI interactive states
    dialog_search_text[0] = '\0';
    dialog_search_cursor = 0;
    
    dialog_file_name[0] = '\0';
    dialog_filename_cursor = 0;
    
    // Sync the visual address text to the new validated directory!
    strcpy(dialog_address_text, current_dir);
    dialog_address_cursor = strlen(dialog_address_text);
    active_input_field = 0; 

    // Auto-clear UI interactive states
    dialog_search_text[0] = '\0';
    dialog_search_cursor = 0;
    
    // --- NEW SAVE MODE PROTECTION ---
    if (!browser_save_mode) {
        dialog_file_name[0] = '\0';
        dialog_filename_cursor = 0;
        active_input_field = 0; 
    } else {
        active_input_field = 4; // Keep focus on the filename box while navigating!
    }
    // --------------------------------
    
    // Sync the visual address text to the new validated directory!
    strcpy(dialog_address_text, current_dir);
    dialog_address_cursor = strlen(dialog_address_text);
    
    filter_browser_files();
}

void write_wav_header(FILE *f, int sample_rate, int channels, int bit_depth, int data_size) {
    unsigned int byterate = sample_rate * channels * (bit_depth / 8); unsigned short block_align = channels * (bit_depth / 8); unsigned int overall_size = data_size + 36; unsigned int fmt_len = 16; unsigned short fmt_type = 1;
    fwrite("RIFF", 1, 4, f); fwrite(&overall_size, 4, 1, f); fwrite("WAVE", 1, 4, f); fwrite("fmt ", 1, 4, f); fwrite(&fmt_len, 4, 1, f); fwrite(&fmt_type, 2, 1, f); fwrite(&channels, 2, 1, f); fwrite(&sample_rate, 4, 1, f); fwrite(&byterate, 4, 1, f); fwrite(&block_align, 2, 1, f); fwrite(&bit_depth, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_size, 4, 1, f);
}

// --- NATIVE WAV SUPPORT GLOBALS ---
int file_is_native_wav = 0;
int current_bitdepth = 16;
long wav_data_size = 0;
// ----------------------------------

long parse_wav_header(const char* filename, long *out_offset, int *out_channels, int *out_bitdepth, long *out_data_size) {
    FILE* file = fopen(filename, "rb"); 
    if (!file) return 0; 
    
    unsigned char header[12];
    if (fread(header, 1, 12, file) == 12) {
        if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
            header[8] == 'W' && header[9] == 'A' && header[10] == 'V' && header[11] == 'E') {
            
            unsigned char chunk[8];
            long sr = 0;
            while (fread(chunk, 1, 8, file) == 8) {
                long chunk_size = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | (chunk[7] << 24);
                if (chunk[0] == 'f' && chunk[1] == 'm' && chunk[2] == 't' && chunk[3] == ' ') {
                    unsigned char fmt[16];
                    fread(fmt, 1, 16, file);
                    *out_channels = fmt[2] | (fmt[3] << 8);
                    sr = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
                    *out_bitdepth = fmt[14] | (fmt[15] << 8);
                    if (chunk_size > 16) fseek(file, chunk_size - 16 + (chunk_size % 2), SEEK_CUR);
                }
                else if (chunk[0] == 'd' && chunk[1] == 'a' && chunk[2] == 't' && chunk[3] == 'a') {
                    *out_offset = ftell(file);
                    *out_data_size = chunk_size;
                    fclose(file);
                    return sr;
                } else {
                    fseek(file, chunk_size + (chunk_size % 2), SEEK_CUR);
                }
            }
        }
    }
    fclose(file); return 0;
}

long parse_mp3_header(const char* filename, long *out_offset, int *out_channels) {
    FILE* file = fopen(filename, "rb"); 
    if (!file) return 0; 
    
    unsigned char buffer[10];
    if (fread(buffer, 1, 10, file) == 10) {
        if (buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
            long tag_size = ((buffer[6] & 0x7F) << 21) | ((buffer[7] & 0x7F) << 14) | ((buffer[8] & 0x7F) << 7) | (buffer[9] & 0x7F);
            fseek(file, tag_size + 10, SEEK_SET);
        } else { fseek(file, 0, SEEK_SET); }
    } else { fseek(file, 0, SEEK_SET); }

    unsigned char sync_buffer[4];
    while (fread(sync_buffer, 1, 1, file) == 1) {
        if (sync_buffer[0] == 0xFF) {
            if (fread(sync_buffer + 1, 1, 3, file) == 3) {
                if ((sync_buffer[1] & 0xE0) == 0xE0) {
                    *out_offset = ftell(file) - 4;
                    int mpeg_v = (sync_buffer[1] >> 3) & 0x03; 
                    int sr_idx = (sync_buffer[2] >> 2) & 0x03;
                    if (sr_idx == 3) { fseek(file, -3, SEEK_CUR); continue; }
                    long rates_mpeg1[3] = {44100, 48000, 32000}; 
                    long rates_mpeg2[3] = {22050, 24000, 16000};
                    long rate = (mpeg_v == 3) ? rates_mpeg1[sr_idx] : rates_mpeg2[sr_idx];
                    *out_channels = ((sync_buffer[3] >> 6) & 3) == 3 ? 1 : 2; 
                    fclose(file); return rate;
                } else { fseek(file, -3, SEEK_CUR); }
            }
        }
    }
    fclose(file); return 0;
}

void parse_id3(const char* filename) {
    has_id3 = 0; 
    memset(id3_title, 0, 31); memset(id3_artist, 0, 31);
    memset(id3_album, 0, 31); memset(id3_year, 0, 5); memset(id3_label, 0, 31);
    memset(id3_album_artist, 0, 31); memset(id3_track, 0, 10); memset(id3_genre, 0, 31);
    memset(id3_composer, 0, 31); memset(id3_copyright, 0, 31); memset(id3_encoded_by, 0, 31);
    memset(id3_bpm, 0, 10); memset(id3_key, 0, 10); 
    memset(id3_remix, 0, 31); memset(id3_subtitle, 0, 31);

    FILE* f = fopen(filename, "rb");
    if (!f) return;

    unsigned char header[10];
    if (fread(header, 1, 10, f) == 10 && header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        int id3_version = header[3]; // <--- Grab the version (3 = v2.3, 4 = v2.4)
        long tag_size = ((header[6] & 0x7F) << 21) | ((header[7] & 0x7F) << 14) | ((header[8] & 0x7F) << 7) | (header[9] & 0x7F);
        long bytes_read = 0;
        
        // --- NEW: Skip Extended Header if it exists! ---
        if (header[5] & 0x40) { 
            unsigned char ext_head[4];
            if (fread(ext_head, 1, 4, f) == 4) {
                long ext_size = 0;
                if (id3_version == 4) ext_size = ((ext_head[0] & 0x7F) << 21) | ((ext_head[1] & 0x7F) << 14) | ((ext_head[2] & 0x7F) << 7) | (ext_head[3] & 0x7F);
                else ext_size = (ext_head[0] << 24) | (ext_head[1] << 16) | (ext_head[2] << 8) | ext_head[3];
                fseek(f, ext_size - 4, SEEK_CUR);
                bytes_read += ext_size;
            }
        }
        
        while (bytes_read < tag_size) { 
            if (id3_version < 3) break; // We only parse v2.3 and v2.4 safely
            
            unsigned char frame_head[10];
            if (fread(frame_head, 1, 10, f) != 10) break;
            bytes_read += 10;
            if (frame_head[0] == 0) break; // Hit padding, stop reading!
            
            // --- NEW: Sync-safe math for v2.4 vs Standard math for v2.3 ---
            long frame_size = 0;
            if (id3_version == 4) {
                frame_size = ((frame_head[4] & 0x7F) << 21) | ((frame_head[5] & 0x7F) << 14) | ((frame_head[6] & 0x7F) << 7) | (frame_head[7] & 0x7F);
            } else {
                frame_size = (frame_head[4] << 24) | (frame_head[5] << 16) | (frame_head[6] << 8) | frame_head[7];
            }
            
            if (frame_size <= 0 || frame_size > 20000000) break; // Failsafe
            
            int is_target = (strncmp((char*)frame_head, "TIT2", 4) == 0 || strncmp((char*)frame_head, "TPE1", 4) == 0 || 
                             strncmp((char*)frame_head, "TALB", 4) == 0 || strncmp((char*)frame_head, "TYER", 4) == 0 || 
                             strncmp((char*)frame_head, "TDRC", 4) == 0 || strncmp((char*)frame_head, "TPUB", 4) == 0 ||
                             strncmp((char*)frame_head, "TPE2", 4) == 0 || strncmp((char*)frame_head, "TRCK", 4) == 0 ||
                             strncmp((char*)frame_head, "TCON", 4) == 0 || strncmp((char*)frame_head, "TCOM", 4) == 0 ||
                             strncmp((char*)frame_head, "TCOP", 4) == 0 || strncmp((char*)frame_head, "TENC", 4) == 0 ||
                             strncmp((char*)frame_head, "TBPM", 4) == 0 || strncmp((char*)frame_head, "TKEY", 4) == 0 ||
                             strncmp((char*)frame_head, "TPE4", 4) == 0 || strncmp((char*)frame_head, "TIT3", 4) == 0);
                             
            if (is_target) {
                unsigned char encoding; fread(&encoding, 1, 1, f); 
                char temp_buf[128] = {0}; 
                int to_read = frame_size - 1; 
                if (to_read > 120) to_read = 120; 
                fread(temp_buf, 1, to_read, f);
                
                char clean_buf[31] = {0}; int c_idx = 0;
                for (int i = 0; i < to_read && c_idx < 30; i++) {
                    if (temp_buf[i] >= 32 && temp_buf[i] <= 126) clean_buf[c_idx++] = temp_buf[i];
                }

                if (strncmp((char*)frame_head, "TIT2", 4) == 0 && strlen(id3_title) == 0) strcpy(id3_title, clean_buf);
                else if (strncmp((char*)frame_head, "TPE1", 4) == 0 && strlen(id3_artist) == 0) strcpy(id3_artist, clean_buf);
                else if (strncmp((char*)frame_head, "TALB", 4) == 0 && strlen(id3_album) == 0) strcpy(id3_album, clean_buf);
                else if ((strncmp((char*)frame_head, "TYER", 4) == 0 || strncmp((char*)frame_head, "TDRC", 4) == 0) && strlen(id3_year) == 0) { strncpy(id3_year, clean_buf, 4); id3_year[4] = '\0'; }
                else if (strncmp((char*)frame_head, "TPUB", 4) == 0 && strlen(id3_label) == 0) strcpy(id3_label, clean_buf);
                else if (strncmp((char*)frame_head, "TPE2", 4) == 0 && strlen(id3_album_artist) == 0) strcpy(id3_album_artist, clean_buf);
                else if (strncmp((char*)frame_head, "TRCK", 4) == 0 && strlen(id3_track) == 0) { strncpy(id3_track, clean_buf, 9); id3_track[9] = '\0'; }
                else if (strncmp((char*)frame_head, "TCON", 4) == 0 && strlen(id3_genre) == 0) strcpy(id3_genre, clean_buf);
                else if (strncmp((char*)frame_head, "TCOM", 4) == 0 && strlen(id3_composer) == 0) strcpy(id3_composer, clean_buf);
                else if (strncmp((char*)frame_head, "TCOP", 4) == 0 && strlen(id3_copyright) == 0) strcpy(id3_copyright, clean_buf);
                else if (strncmp((char*)frame_head, "TENC", 4) == 0 && strlen(id3_encoded_by) == 0) strcpy(id3_encoded_by, clean_buf);
                else if (strncmp((char*)frame_head, "TBPM", 4) == 0 && strlen(id3_bpm) == 0) { strncpy(id3_bpm, clean_buf, 9); id3_bpm[9] = '\0'; }
                else if (strncmp((char*)frame_head, "TKEY", 4) == 0 && strlen(id3_key) == 0) { strncpy(id3_key, clean_buf, 9); id3_key[9] = '\0'; }
                else if (strncmp((char*)frame_head, "TPE4", 4) == 0 && strlen(id3_remix) == 0) strcpy(id3_remix, clean_buf);
                else if (strncmp((char*)frame_head, "TIT3", 4) == 0 && strlen(id3_subtitle) == 0) strcpy(id3_subtitle, clean_buf);

                fseek(f, (frame_size - 1) - to_read, SEEK_CUR); 
            } else fseek(f, frame_size, SEEK_CUR); 
            bytes_read += frame_size;
        }
        if (strlen(id3_title) > 0 || strlen(id3_artist) > 0) has_id3 = 1;
    }

    if (!has_id3) {
        fseek(f, -128, SEEK_END); char tag[3];
        if (fread(tag, 1, 3, f) == 3 && tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
            has_id3 = 1;
            fread(id3_title, 1, 30, f); fread(id3_artist, 1, 30, f); fread(id3_album, 1, 30, f); fread(id3_year, 1, 4, f);
            for (int i = 29; i >= 0; i--) { if (id3_title[i] == ' ' || id3_title[i] < 32) id3_title[i] = '\0'; else if (id3_title[i] != '\0') break; }
            for (int i = 29; i >= 0; i--) { if (id3_artist[i] == ' ' || id3_artist[i] < 32) id3_artist[i] = '\0'; else if (id3_artist[i] != '\0') break; }
            for (int i = 29; i >= 0; i--) { if (id3_album[i] == ' ' || id3_album[i] < 32) id3_album[i] = '\0'; else if (id3_album[i] != '\0') break; }
            id3_year[4] = '\0';
        }
    }
    fclose(f);
}

void write_dsp(unsigned char command) {
    while (inportb(DSP_WRITE) & 0x80); 
    outportb(DSP_WRITE, command);
}

unsigned char read_dsp() {
    while (!(inportb(DSP_STATUS) & 0x80)); 
    return inportb(DSP_READ);
}

void get_dsp_version(int *major, int *minor) {
    write_dsp(0xE1); 
    *major = read_dsp();
    *minor = read_dsp();
}

int reset_dsp() {
    outportb(DSP_RESET, 1); delay(10); outportb(DSP_RESET, 0); 
    int timeout = 1000;
    while (timeout > 0) {
        if (inportb(DSP_STATUS) & 0x80) { if (inportb(DSP_READ) == 0xAA) return 1; }
        timeout--;
    }
    return 0; 
}

int setup_dma() {
    dos_buffer.size = (active_buffer_size * 2) / 16; 
    if (_go32_dpmi_allocate_dos_memory(&dos_buffer) != 0) return 0;
    unsigned long phys = dos_buffer.rm_segment * 16;
    unsigned long page1 = phys / 131072UL;
    physical_addr = phys;

    if (page1 != (phys + active_buffer_size - 1) / 131072UL) physical_addr = (page1 + 1) * 131072UL;

    unsigned char zero[4096]; memset(zero, (active_bitdepth == 8) ? 128 : 0, 4096); 
    for (int i = 0; i < active_buffer_size; i += 4096) dosmemput(zero, 4096, physical_addr + i);

    if (active_bitdepth == 16) {
        outportb(DMA16_MASK, 0x05); outportb(DMA16_CLEAR, 0x00); outportb(DMA16_MODE, 0x59); 
        unsigned long dma_addr = (physical_addr >> 1) & 0xFFFF;
        outportb(DMA16_ADDR, (unsigned char)(dma_addr & 0xFF)); outportb(DMA16_ADDR, (unsigned char)((dma_addr >> 8) & 0xFF));
        outportb(DMA16_PAGE, (unsigned char)((physical_addr >> 16) & 0xFE));
        unsigned int dma_len = (unsigned int)((active_buffer_size / 2) - 1); 
        outportb(DMA16_COUNT, (unsigned char)(dma_len & 0xFF)); outportb(DMA16_COUNT, (unsigned char)((dma_len >> 8) & 0xFF));
        outportb(DMA16_MASK, 0x01); 
    } else {
        outportb(DMA8_MASK, 0x05); outportb(DMA8_CLEAR, 0x00); 
        if (!has_auto_init) outportb(DMA8_MODE, 0x49); // Single-cycle playback for SB 1.x and DSP 2.00
        else outportb(DMA8_MODE, 0x59);               // Auto-init playback for SB 2.01+ / Pro
        
        unsigned long dma_addr = physical_addr & 0xFFFF; 
        outportb(DMA8_ADDR, (unsigned char)(dma_addr & 0xFF)); outportb(DMA8_ADDR, (unsigned char)((dma_addr >> 8) & 0xFF));
        outportb(DMA8_PAGE, (unsigned char)((physical_addr >> 16) & 0xFF));
        unsigned int dma_len = (unsigned int)(active_buffer_size - 1); 
        outportb(DMA8_COUNT, (unsigned char)(dma_len & 0xFF)); outportb(DMA8_COUNT, (unsigned char)((dma_len >> 8) & 0xFF));
        outportb(DMA8_MASK, 0x01); 
    }
    return 1;
}


void set_volume(int vol) {
    if(vol < 0) vol = 0; if(vol > 100) vol = 100; master_volume = vol;
    if (!global_is_pc_speaker) {
        if (dsp_major >= 4) { // Sound Blaster 16
            unsigned char hw_vol = (unsigned char)(((vol * 31) / 100) << 3);
            outportb(MIXER_ADDR, 0x30); outportb(MIXER_DATA, hw_vol);
            outportb(MIXER_ADDR, 0x31); outportb(MIXER_DATA, hw_vol);
        } else if (dsp_major == 3) { // Sound Blaster Pro 2.0 / ESS Clones
            unsigned char hw_vol = (unsigned char)((vol * 15) / 100);
            unsigned char mix_vol = (hw_vol << 4) | hw_vol; // Pack L/R nibbles
            outportb(MIXER_ADDR, 0x22); outportb(MIXER_DATA, mix_vol); // Master Vol
            outportb(MIXER_ADDR, 0x04); outportb(MIXER_DATA, mix_vol); // Voice Vol
        }
        // SB 1.x and 2.0 typically rely on hardware dials, so we just ignore them here safely.
    }
}

void speaker_handler() {
    outportb(0x20, 0x20); interrupt_count++;
    unsigned char sample = pc_speaker_dma[dma_play_pos]; dma_play_pos++;
    
    if (dma_play_pos == active_buffer_size / 2) refill_request = 1;
    else if (dma_play_pos >= active_buffer_size) { refill_request = 2; dma_play_pos = 0; }
    
    int s = sample - 128;
    s = (s * pc_speaker_overdrive) / 100; 
    if (s > 127) s = 127; if (s < -128) s = -128;
    
    s = (s * master_volume) / 100;
    sample = (unsigned char)(s + 128);
    
    unsigned int pwm = (sample * pit_divisor) >> 8; if(pwm == 0) pwm = 1; 
    outportb(0x42, pwm); 
}
void speaker_handler_end() {}

void sb_handler() {
    interrupt_count++;
    
    // Ack interrupts based on hardware capabilities
    inportb(DSP_STATUS); // Ack standard 8-bit
    if (active_bitdepth == 16) inportb(DSP_16_ACK); // Ack 16-bit
    
    // DSP 1.x & 2.00 workaround: Single-cycle DMA must be continually re-issued!
    if (!global_is_pc_speaker && !is_sb16 && !has_auto_init) {
        unsigned int dma_len = (active_buffer_size / 2) - 1;
        unsigned long dma_addr = physical_addr + ((interrupt_count % 2 == 1) ? (active_buffer_size / 2) : 0);
        
        outportb(DMA8_MASK, 0x05); 
        outportb(DMA8_CLEAR, 0x00); 
        outportb(DMA8_MODE, 0x49); 
        outportb(DMA8_ADDR, (unsigned char)(dma_addr & 0xFF)); 
        outportb(DMA8_ADDR, (unsigned char)((dma_addr >> 8) & 0xFF));
        outportb(DMA8_COUNT, (unsigned char)(dma_len & 0xFF)); 
        outportb(DMA8_COUNT, (unsigned char)((dma_len >> 8) & 0xFF));
        outportb(DMA8_MASK, 0x01); 

        write_dsp(0x14); 
        write_dsp((unsigned char)(dma_len & 0xFF)); 
        write_dsp((unsigned char)((dma_len >> 8) & 0xFF));
    }
    
    outportb(0x20, 0x20); // End of Interrupt to PIC
    
    if (interrupt_count % 2 == 1) refill_request = 1; else refill_request = 2;
}
void sb_handler_end() {}

// --- DYNAMIC VIDEO MEMORY ---
unsigned long video_address = 0xB8000; // Defaults to VGA
int vsync_port = 0x3DA;                // Defaults to VGA VSYNC port
int vsync_mask = 0x08;                 // Defaults to VGA VSYNC bit

void calibrate_vsync(int is_mda) {
    unsigned char mda_toggles = 0, vga_toggles = 0;
    unsigned char last_mda = inportb(0x3BA), last_vga = inportb(0x3DA);
    
    clock_t start_time = clock();
    while ((clock() - start_time) < (CLOCKS_PER_SEC / 10)) { // 100ms sample window
        unsigned char mda_val = inportb(0x3BA);
        unsigned char vga_val = inportb(0x3DA);
        mda_toggles |= (mda_val ^ last_mda);
        vga_toggles |= (vga_val ^ last_vga);
        last_mda = mda_val; last_vga = vga_val;
    }

    if (is_mda) {
        vsync_port = 0x3BA;
        if (mda_toggles & 0x80) vsync_mask = 0x80;      // Real Hercules hardware
        else if (mda_toggles & 0x08) vsync_mask = 0x08; // DOSBox-X quirk
        else vsync_port = 0;                            // Pure IBM MDA (No VSYNC)
    } else {
        vsync_port = 0x3DA;
        if (vga_toggles & 0x08) vsync_mask = 0x08;      // Standard VGA/EGA
        else vsync_port = 0;                            // Failsafe (Dead carrier)
    }
}

void wait_vsync() {
    if (vsync_port == 0) return; 
    while (inportb(vsync_port) & vsync_mask);
    while (!(inportb(vsync_port) & vsync_mask));
}

/* VRAM diffing engine */
void set_char(int r, int c, char ch, unsigned char color) {
    // --- UNIVERSAL ANSI INTERCEPTOR ---
    if (setting_ansi_mode == 0) {
        unsigned char uc = (unsigned char)ch;
        switch(uc) {
            // Box drawing borders
            case 0xDA: case 0xBF: case 0xC0: case 0xD9: 
            case 0xC3: case 0xB4: case 0xC5:
                ch = '+'; break;
            case 0xC4: case 0xC2: case 0xC1:
                ch = '-'; break;
            case 0xB3: 
                ch = '|'; break;
            
            // UI Elements & Blocks
            case 0xDB: // 219 Solid block (Visualizer, Progress Bar, Scrollbars)
                ch = '#'; break;
            case 0xB0: // 176 Light shade (Scrollbar / Progress empty)
                ch = ' '; break;
            case 177:  // Medium shade (Progress glow)
                ch = '>'; break;
            case 221:  // Cursor
                ch = '|'; break;
            
            // Icons & Symbols
            case 0x10: // 16 Right arrow (Play, Fwd)
                ch = '>'; break;
            case 0x11: // 17 Left arrow (Rew)
                ch = '<'; break;
            case 30:   // Up arrow
                ch = '^'; break;
            case 31:   // Down arrow
                ch = 'v'; break;
            case 15:   // Star (Settings)
                ch = '*'; break;
            case 0xFE: // 254 Square (Stop)
                ch = '#'; break;
            case 0xEC: // 236 Infinity/Loop
                ch = 'O'; break;
            case 0xF0: // 240 List/Menu (Playlist)
                ch = '='; break;
            case 27:   // Left arrow
                ch = '<'; break;
        }
    }
    // ----------------------------------

    int offset = (r * 80 + c) * 2;
    unsigned char curr_ch = _farpeekb(_dos_ds, video_address + offset);
    unsigned char curr_col = _farpeekb(_dos_ds, video_address + offset + 1);
    
    if (curr_ch != (unsigned char)ch || curr_col != color) {
        _farpokeb(_dos_ds, video_address + offset, ch);
        _farpokeb(_dos_ds, video_address + offset + 1, color);
    }
}

void print_text(int r, int c, const char* str, unsigned char color) {
    while(*str) set_char(r, c++, *str++, color);
}

void print_fmt(int r, int c, unsigned char color, const char* fmt, ...) {
    char buf[1024]; 
    va_list args; va_start(args, fmt); vsprintf(buf, fmt, args); va_end(args);
    print_text(r, c, buf, color);
}

void draw_frame(int c1, int r1, int c2, int r2, unsigned char color) {
    set_char(r1, c1, 0xDA, color); for(int i=c1+1; i<c2; i++) set_char(r1, i, 0xC4, color); set_char(r1, c2, 0xBF, color);
    for(int j=r1+1; j<r2; j++) { set_char(j, c1, 0xB3, color); set_char(j, c2, 0xB3, color); }
    set_char(r2, c1, 0xC0, color); for(int i=c1+1; i<c2; i++) set_char(r2, i, 0xC4, color); set_char(r2, c2, 0xD9, color);
}

unsigned char decode_char(char ch) {
    if (setting_ansi_mode == 0) {
        switch(ch) {
            case '|': case '!': case '@': case '$': case '?': return '|';
            case '*': case '=': case '~': case '^': case '_': return '-';
            case '[': case ']': case '{': case '}': case '+': case '`': case '%': case '<': case '&': case '>': return '+';
            case '1': return '^'; case '2': return 'v'; case '3': return '*'; case '4': return '<'; case '5': return '>';
            case '6': return '<';
            default: return (unsigned char)ch;
        }
    } else {
        switch(ch) {
            case '|': return 179;
            case '*': return 196; 
            case '[': return 218; 
            case ']': return 191;
            case '{': return 192; 
            case '}': return 217; 
            case '^': return 194; 
            case '_': return 193;
            case '@': return 195; 
            case '$': return 180; 
            case '+': return 197; 
            case '!': return 186;
            case '=': return 205; 
            case '~': return 209; 
            case '`': return 201; 
            case '%': return 187;
            case '<': return 200; 
            case '&': return 207; 
            case '>': return 188; 
            case '?': return 219;
            case '1': return 30;  
            case '2': return 31;  
            case '3': return 15;  
            case '4': return 17;
            case '5': return 16;
            case '6': return 27; 
            default: return (unsigned char)ch;
        }
    }
}

void print_mapped_str(int r, int c, const char* str, unsigned char color) {
    while(*str) set_char(r, c++, decode_char(*str++), color);
}

const char* get_basename(const char* path) {
    if (!path) return "None";
    const char* last_slash = strrchr(path, '/');
    const char* last_bslash = strrchr(path, '\\');
    const char* slash = (last_slash > last_bslash) ? last_slash : last_bslash;
    return slash ? slash + 1 : path;
}


// --- SETTINGS STUBS & WRAPPERS ---
unsigned char make_color(unsigned char bg, unsigned char fg) { 
    return (bg << 4) | fg; 
}

unsigned char get_theme_color(int type) {
    int bg = setting_bg_color;
    int fg = setting_fg_color;

    // --- MONOCHROME MODE: Force Black & Grey Palette ---
    if (!setting_color) {
        bg = (setting_bg_color == 7) ? 7 : 0;
        fg = (bg == 7) ? 0 : 7;
        
        switch(type) {
            case 0: return make_color(bg, fg);
            case 1: return make_color(fg, bg); // Highlight = Invert both BG and FG perfectly!
            case 2: return make_color(fg, bg); // Critical = Invert both BG and FG perfectly!
            case 3: return make_color(bg, fg);  // Dim = Standard BG, dark grey text
            case 4: return make_color(bg, fg); // Accent = Standard
            default: return make_color(bg, fg);
        }
    }

    // --- STANDARD COLOR MODE ---
    switch(type) {
        case 0: return make_color(bg, fg);
        case 1: { 
            int hl_bg = setting_sel_color;
            int hl_fg = 15; 
            return make_color(hl_bg, hl_fg);
        }
        case 2: { 
            int cr_bg = setting_crit_color;
            int cr_fg = 15; 
            return make_color(cr_bg, cr_fg);
        }
        case 3: return make_color(bg, 8); 
        case 4: { 
            int ac_fg = 11;
            if (bg == 7 || bg == 3 || bg == 2 || bg == 6) ac_fg = 0; 
            return make_color(bg, ac_fg);
        }
        default: return 0x07;
    }
}

unsigned char get_bg_color() { return get_theme_color(0); }
unsigned char get_hl_color() { return get_theme_color(1); }
unsigned char get_cr_color() { return get_theme_color(2); }
unsigned char get_dim_color() { return get_theme_color(3); }
unsigned char get_accent_color() { return get_theme_color(4); }

void toggle_theme_color(int* target_color, int col_idx) {
    if (col_idx == 0) *target_color = 0; 
    else if (col_idx == 1) *target_color = 7; 
    else if (setting_color) {
        int mask = (col_idx == 2) ? 1 : ((col_idx == 3) ? 2 : 4);
        if (*target_color == 0 || *target_color == 7) *target_color = mask;
        else *target_color ^= mask;
    }
}

void enforce_theme_contrast(int primary_trigger) {
    if (primary_trigger == 1) {
        if (setting_bg_color == setting_fg_color) setting_fg_color = (setting_bg_color == 7) ? 0 : 7;
    } else if (primary_trigger == 2) {
        if (setting_fg_color == setting_bg_color) setting_bg_color = (setting_fg_color == 7) ? 0 : 7;
    }
    
    if (setting_bg_color == setting_sel_color) setting_sel_color = (setting_bg_color == 7) ? 1 : 7;
    if (setting_bg_color == setting_crit_color) setting_crit_color = (setting_bg_color == 7) ? 4 : 7;
}

void print_str(int r, int c, const char* str, unsigned char color) {
    print_text(r, c, str, color);
}

void print_theme_line(int y, const char* label, int current_val, int use_color, unsigned char bg, unsigned char hl, int mx, int my, int hover_on) {
    char buf[120];
    if (current_val == 15) current_val = 7;
    
    char mk_blk = (current_val == 0) ? '*' : ' ';
    char mk_wht = (current_val == 7) ? '*' : ' '; 
    char mk_blu = (current_val != 0 && current_val != 7 && (current_val & 1)) ? 'V' : ' ';
    char mk_grn = (current_val != 0 && current_val != 7 && (current_val & 2)) ? 'V' : ' ';
    char mk_red = (current_val != 0 && current_val != 7 && (current_val & 4)) ? 'V' : ' ';

    if (setting_ansi_mode == 1) {
        mk_blk = (current_val == 0) ? '\x07' : ' ';  
        mk_wht = (current_val == 7) ? '\x07' : ' ';  
        mk_blu = (current_val != 0 && current_val != 7 && (current_val & 1)) ? '\xFB' : ' ';  
        mk_grn = (current_val != 0 && current_val != 7 && (current_val & 2)) ? '\xFB' : ' ';
        mk_red = (current_val != 0 && current_val != 7 && (current_val & 4)) ? '\xFB' : ' ';
    }

    if (use_color) {
        sprintf(buf, "%-20s - Black (%c), White (%c), Blue [%c], Green [%c], Red [%c]", label, mk_blk, mk_wht, mk_blu, mk_grn, mk_red);
    } else {
        sprintf(buf, "%-20s - Black (%c), White (%c)", label, mk_blk, mk_wht);
    }
    
    print_text(y, 4, buf, bg);
    
    // --- NEW SAFE WIPE LOGIC ---
    // Clears exactly up to the right border (column 78) without wrapping!
    if (!use_color) {
        for (int i = 4 + strlen(buf); i < 79; i++) {
            set_char(y, i, ' ', bg);
        }
    }
    // ---------------------------

    if (hover_on && my == y) {
        char temp_hover[32]; 
        if (mx >= 27 && mx <= 35) { sprintf(temp_hover, "Black (%c)", mk_blk); print_text(y, 27, temp_hover, hl); }
        else if (mx >= 38 && mx <= 46) { sprintf(temp_hover, "White (%c)", mk_wht); print_text(y, 38, temp_hover, hl); }
        else if (use_color) {
            if (mx >= 49 && mx <= 56) { sprintf(temp_hover, "Blue [%c]", mk_blu); print_text(y, 49, temp_hover, hl); }
            else if (mx >= 59 && mx <= 67) { sprintf(temp_hover, "Green [%c]", mk_grn); print_text(y, 59, temp_hover, hl); }
            else if (mx >= 70 && mx <= 76) { sprintf(temp_hover, "Red [%c]", mk_red); print_text(y, 70, temp_hover, hl); }
        }
    }
}

void clear_inner_ui() {
    unsigned char bg = get_bg_color();
    for (int r = 3; r <= 17; r++) {
        for (int c = 1; c < 79; c++) set_char(r, c, ' ', bg);
    }
}

void draw_player_btn(int x, int w, const char* icon, unsigned char border_col, unsigned char icon_col) {
    set_char(21, x, 0xDA, border_col);
     for(int i=1; i<w-1; i++) set_char(21, x+i, 0xC4, border_col); set_char(21, x+w-1, 0xBF, border_col);
    set_char(22, x, 0xB3, border_col); print_text(22, x+1, icon, icon_col); set_char(22, x+w-1, 0xB3, border_col);
    set_char(23, x, 0xC0, border_col);
    for(int i=1; i<w-1; i++) set_char(23, x+i, 0xC4, border_col); set_char(23, x+w-1, 0xD9, border_col);
}

void draw_player_btn2(int x, int w, const char* icon, unsigned char border_col, unsigned char icon_col) {
    set_char(21, x, 0xDA, border_col);
     for(int i=1; i<w-1; i++) set_char(21, x+i, 0xC4, border_col); set_char(21, x+w-1, 0xBF, border_col);
    set_char(22, x, 0xB3, border_col); print_text(22, x+1, icon, icon_col); set_char(22, x+w-1, 0xB3, border_col);
    set_char(23, x, 0xC0, border_col);
    for(int i=1; i<w-1; i++) set_char(23, x+i, 0xC4, border_col); set_char(23, x+w-1, 0xD9, border_col);
}

void init_ui(const char* filename) {
    _setcursortype(_NOCURSOR); 
    unsigned char bg = get_bg_color();
    unsigned char ac = get_accent_color();
    force_ui_redraw = 1;
    
    for(int r = 0; r < 25; r++) {
        for(int c = 0; c < 80; c++) set_char(r, c, ' ', bg);
    }

    if (ui_view == 0 || ui_view == 3 || ui_view == 4) {
        const char* base_filename = get_basename(filename);
        
        draw_frame(0, 0, 79, 24, bg);
        set_char(2, 0, 0xC3, bg); for(int i=1; i<79; i++) set_char(2, i, 0xC4, bg); set_char(2, 79, 0xB4, bg);
        if (strlen(base_filename) < 45){
            print_fmt(1, 2, bg, "File \xB3 Audio \xB3 Visual \xB3 %s", base_filename); 
        }
        else if (strlen(base_filename) >= 49){
            print_fmt(1, 2, bg, "File \xB3 Audio \xB3 Visual \xB3 %.45s...", base_filename); 
        }
        print_text(1, 75, "\xB3 X ", bg); 
        
        print_text(0, 7, "\xC2", bg); print_text(2, 7, "\xC1", bg);
        print_text(0, 15, "\xC2", bg); print_text(2, 15, "\xC1", bg);
        print_text(0, 24, "\xC2", bg); print_text(2, 24, "\xC1", bg);
        print_text(0, 75, "\xC2", bg); print_text(2, 75, "\xC1", bg);

        if (ui_view == 0) { // Only draw ID3 logic in Player view
            char full_title[128] = {0};
            
            // 1. Build the full string safely
            if (has_id3 && (strlen(id3_artist) > 0 || strlen(id3_title) > 0)) {
                if (strlen(id3_artist) > 0 && strlen(id3_title) > 0) sprintf(full_title, "%s - %s", id3_artist, id3_title);
                else if (strlen(id3_title) > 0) strcpy(full_title, id3_title);
                else strcpy(full_title, id3_artist);
            } else {
                strcpy(full_title, base_filename);
            }

            int max_len = 62; // The maximum characters that can fit on one line before hitting the right border
            
            // 2. Wrap if necessary
            if (strlen(full_title) <= max_len) {
                print_fmt(4, 2, ac, "Now Playing: %s", full_title);
            } else {
                int break_idx = max_len;
                
                // Walk backward to find a clean space or hyphen to break on
                while (break_idx > 30 && full_title[break_idx] != ' ' && full_title[break_idx] != '-') break_idx--;
                if (break_idx == 30) break_idx = max_len; // Failsafe hard break if it's one massive word

                char line1[65] = {0};
                strncpy(line1, full_title, break_idx);
                print_fmt(4, 2, ac, "Now Playing: %s", line1);

                int start2 = break_idx;
                if (full_title[start2] == ' ') start2++; // Skip the leading space on line 2 so it doesn't indent weirdly

                char line2[65] = {0};
                strncpy(line2, full_title + start2, max_len); 
                
                // If it STILL overflows line 2, safely append an ellipsis
                if (strlen(full_title + start2) > max_len) {
                    strcpy(line2 + max_len - 3, "...");
                }
                
                // Print Line 2 starting at Column 15 to perfectly align under the title text!
                print_fmt(5, 15, ac, "%s", line2); 
            }
        }
        
        set_char(18, 0, 0xC3, bg);
        for(int i=1; i<=7; i++) set_char(18, i, 0xC4, bg); set_char(18, 8, 0xC2, bg); 
        for(int i=9; i<=70; i++) set_char(18, i, 0xC4, bg); set_char(18, 71, 0xC2, bg); 
        for(int i=72; i<=78; i++) set_char(18, i, 0xC4, bg); set_char(18, 79, 0xB4, bg); 
        set_char(19, 0, 0xB3, bg); 
        print_text(19, 2, "00:00", bg); 
        set_char(19, 8, 0xB3, bg); 
        set_char(19, 71, 0xB3, bg); 
        print_text(19, 73, "00:00", bg); 
        set_char(19, 79, 0xB3, bg); 
        set_char(20, 0, 0xC3, bg); 
        for(int i=1; i<=7; i++) set_char(20, i, 0xC4, bg); set_char(20, 8, 0xC1, bg); 
        for(int i=9; i<=70; i++) set_char(20, i, 0xC4, bg); set_char(20, 71, 0xC1, bg); 
        for(int i=72; i<=78; i++) set_char(20, i, 0xC4, bg); set_char(20, 79, 0xB4, bg); 
        
        force_ui_redraw = 1;
    }
}

unsigned char mouse_arrow[16] = { 0x00, 0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF, 0xFF, 0xF8, 0xEC, 0xCC, 0x86, 0x02 };
void load_cursor_glyph() { __dpmi_regs regs; dosmemput(mouse_arrow, 16, __tb); regs.x.ax = 0x1100; regs.x.bx = 0x1000; regs.x.cx = 0x0001; regs.x.dx = CURSOR_CHAR; regs.x.es = __tb / 16; regs.x.bp = __tb & 15; __dpmi_int(0x10, &regs); }
void show_mouse() { union REGS regs; regs.x.ax = SHOW_MOUSE; int86(MOUSE_INT, &regs, &regs); }
void hide_mouse() { union REGS regs; regs.x.ax = HIDE_MOUSE; int86(MOUSE_INT, &regs, &regs); }
int init_mouse() { union REGS regs; regs.x.ax = INIT_MOUSE; int86(MOUSE_INT, &regs, &regs); if (regs.x.ax == 0xFFFF) { load_cursor_glyph(); regs.x.ax = 0x000A; regs.x.bx = 0x0000; regs.x.cx = 0xF000; regs.x.dx = 0x0F00 | CURSOR_CHAR; int86(MOUSE_INT, &regs, &regs); show_mouse(); has_mouse = 1; return 1; } return 0; }
void update_mouse() { union REGS regs; regs.x.ax = GET_MOUSE_STATUS; int86(MOUSE_INT, &regs, &regs); mouse_x = regs.x.cx / 8; mouse_y = regs.x.dx / 8; mouse_left = (regs.x.bx & 1); mouse_right = (regs.x.bx & 2); }

// --- UI DRAWING FUNCTIONS ---
void draw_visualizer() {
    if (visualizer_mode == 0 || ui_view != 0) return; 

    // --- GLOBAL FPS CAP ---
    // If capped, we just exit. The VRAM engine holds the old frame perfectly!
    static clock_t last_vis_time = 0;
    if (vis_fps_cap > 0) {
        clock_t now = clock();
        clock_t limit = (vis_fps_cap == 1) ? (CLOCKS_PER_SEC / 30) : (CLOCKS_PER_SEC / 15);
        if ((now - last_vis_time) < limit) return;
        last_vis_time = now;
    }
    // ----------------------

    unsigned char bg = get_bg_color();
    unsigned char dim = get_dim_color();
    unsigned char ac = get_accent_color();
    
    int mouse_in_vis = (has_mouse && mouse_y >= 6 && mouse_y <= 16);
    if (mouse_in_vis) hide_mouse();

    if (visualizer_mode == 1) { // bars visualizer
        int max_val = 32768, max_height = 8, start_row = 14, start_col = 15, bar_spacing = 5; 
        int drop_speed = (vis_bar_falloff == 0) ? 500 : ((vis_bar_falloff == 1) ? 1500 : 3500);
        
        for (int i = 0; i < NUM_BANDS; i++) {
            smoothed_spectrum[i] = (smoothed_spectrum[i] * 85 + spectrum_peaks[i] * 15) / 100;
            spectrum_peaks[i] -= drop_speed; if (spectrum_peaks[i] < 0) spectrum_peaks[i] = 0;
            
            int height = (smoothed_spectrum[i] * max_height) / max_val; if (height > max_height) height = max_height;
            int peak_h = (spectrum_peaks[i] * max_height) / max_val; if (peak_h > max_height) peak_h = max_height;
            
            int col = start_col + (i * bar_spacing);
            for (int r = 0; r < max_height; r++) {
                int draw_row = start_row - r; 
                
                // Color Zones mapping to your UI settings
                unsigned char fg = vis_bar_c1; 
                if (r > 3) fg = vis_bar_c2; 
                if (r > 5) fg = vis_bar_c3;
                
                // Dynamic Bar Styles
                char bar_char = 0xDB;
                if (vis_bar_style == 1) { // Shaded Style
                    if (r > 5) bar_char = 178;      // Darkest
                    else if (r > 3) bar_char = 177; // Medium
                    else bar_char = 176;            // Lightest
                }

                // --- THE MOUSE SHADOW HACK ---
                unsigned char current_bg = (bg & 0xF0);
                if (vis_bar_style == 0 && setting_color && fg >= 8) {
                    current_bg = ((fg - 8) << 4); // Map the background to the dark equivalent of the FG
                }
                
                unsigned char color = setting_color ? (current_bg | fg) : ac; 
                // -----------------------------

                if (r < height) { 
                    set_char(draw_row, col, bar_char, color); 
                    set_char(draw_row, col+1, bar_char, color); 
                    set_char(draw_row, col+2, bar_char, color); 
                } 
                else if (vis_bar_peaks && r == peak_h && peak_h > 0) { 
                    // Draw hovering peak cap (220 is the bottom-half block ▄)
                    set_char(draw_row, col, 220, color); 
                    set_char(draw_row, col+1, 220, color); 
                    set_char(draw_row, col+2, 220, color); 
                }
                else { 
                    set_char(draw_row, col, ' ', bg); set_char(draw_row, col+1, ' ', bg); set_char(draw_row, col+2, ' ', bg); 
                }
            }
        }
    }
    else if (visualizer_mode == 2) { 
        int max_val = 32768, bar_width = 60;  
        int drop_speed = (vis_vu_falloff == 0) ? 1000 : ((vis_vu_falloff == 1) ? 2500 : 5000);
        
        smoothed_vu_left = (smoothed_vu_left * 80 + vu_left_peak * 20) / 100; smoothed_vu_right = (smoothed_vu_right * 80 + vu_right_peak * 20) / 100;
        vu_left_peak -= drop_speed; if (vu_left_peak < 0) vu_left_peak = 0; 
        vu_right_peak -= drop_speed; if (vu_right_peak < 0) vu_right_peak = 0;
        
        int left_h = (smoothed_vu_left * bar_width) / max_val; if (left_h > bar_width) left_h = bar_width;
        int right_h = (smoothed_vu_right * bar_width) / max_val; if (right_h > bar_width) right_h = bar_width;
        
        int left_p = (vu_left_peak * bar_width) / max_val; if (left_p > bar_width) left_p = bar_width;
        int right_p = (vu_right_peak * bar_width) / max_val; if (right_p > bar_width) right_p = bar_width;
        
        if (global_out_channels == 1) print_text(9, 7, "C [", bg); else print_text(9, 7, "L [", bg); 
        for (int i = 0; i < bar_width; i++) { 
            // Color Zones mapping to your UI settings
            unsigned char fg = vis_vu_c1; if (i > 35) fg = vis_vu_c2; if (i > 50) fg = vis_vu_c3; 
            
            // --- THE MOUSE SHADOW HACK ---
            unsigned char current_bg = (bg & 0xF0);
            if (setting_color && fg >= 8) {
                current_bg = ((fg - 8) << 4);
            }
            unsigned char color = setting_color ? (current_bg | fg) : ac;
            unsigned char flat_color = setting_color ? ((bg & 0xF0) | fg) : ac; // Prevents the thin '|' from having an ugly background box
            // -----------------------------
            
            if (i < left_h) set_char(9, 10 + i, 0xDB, color); 
            else if (vis_vu_peaks && i == left_p && left_p > 0) set_char(9, 10 + i, '|', flat_color);
            else set_char(9, 10 + i, 0xB0, dim); 
        } 
        print_text(9, 10 + bar_width, "]", bg);
        
        if (global_out_channels == 2) { 
            print_text(11, 7, "R [", bg); 
            for (int i = 0; i < bar_width; i++) { 
                unsigned char fg = vis_vu_c1; if (i > 35) fg = vis_vu_c2; if (i > 50) fg = vis_vu_c3; 
                
                // --- THE MOUSE SHADOW HACK ---
                unsigned char current_bg = (bg & 0xF0);
                if (setting_color && fg >= 8) {
                    current_bg = ((fg - 8) << 4);
                }
                unsigned char color = setting_color ? (current_bg | fg) : ac;
                unsigned char flat_color = setting_color ? ((bg & 0xF0) | fg) : ac;
                // -----------------------------
                
                if (i < right_h) set_char(11, 10 + i, 0xDB, color); 
                else if (vis_vu_peaks && i == right_p && right_p > 0) set_char(11, 10 + i, '|', flat_color);
                else set_char(11, 10 + i, 0xB0, dim); 
            } 
            print_text(11, 10 + bar_width, "]", bg); 
        }
    }
    else if (visualizer_mode == 3) { 
        unsigned char color = setting_color ? ((bg & 0xF0) | 0x0E) : ac;
        unsigned char color2 = setting_color ? ((bg & 0xF0) | 0x0D) : ac;
        
        // --- REFRESH RATE THROTTLE ---
        static clock_t last_dbg_time = 0;
        static int d_irq = 0, d_frm = 0, d_skp = 0, d_lft = 0;
        clock_t now = clock();
        int update_interval = (vis_dbg_refresh == 0) ? 0 : ((vis_dbg_refresh == 1) ? CLOCKS_PER_SEC / 2 : CLOCKS_PER_SEC);
        
        if (now - last_dbg_time >= update_interval) {
            d_irq = interrupt_count; d_frm = frames_decoded; d_skp = buffer_skips; d_lft = pcm_leftover_bytes;
            last_dbg_time = now;
        }
        // -----------------------------
        
        print_text(7, 6, "--- HARDWARE SUBSYSTEM TELEMETRY ---", color); 
        print_fmt(9, 6, bg, "IRQs: %-6d \xB3 Frames: %-6d \xB3 Skips: %-4d \xB3 Leftover: %-6d", d_irq, d_frm, d_skp, d_lft);
        print_fmt(11, 6, bg, "Target Rate : %-8d Hz", global_out_sample_rate); 
        print_fmt(12, 6, bg, "Bit Depth   : %d-bit       ", active_bitdepth);
        print_fmt(13, 6, bg, "Channels    : %-13s", global_out_channels == 2 ? "Stereo" : "Mono"); 
        print_fmt(14, 6, color2, "Engine      : %-15s", global_is_pc_speaker ? "PC Speaker PWM" : "Sound Blaster");
        
        // --- DETAIL LEVEL TOGGLE ---
        if (vis_dbg_detail == 1) { 
            print_fmt(13, 40, bg, "Orig Rate : %-8d Hz", current_sample_rate); 
            
            char dbg_file_type[20];
            if (file_is_native_wav) strcpy(dbg_file_type, "WAV (Native)");
            else if (global_is_486) strcpy(dbg_file_type, "WAV (Cache)");
            else strcpy(dbg_file_type, "MP3 (Stream)");
            
            print_fmt(12, 40, bg, "File Type : %-15s", dbg_file_type); 
            print_fmt(11, 40, bg, "Orig Chans: %-15s", current_channels == 2 ? "Stereo" : "Mono");
        } else {
            // Actively wipe the advanced stats if toggled back to Standard!
            print_text(13, 40, "                       ", bg);
            print_text(12, 40, "                       ", bg);
            print_text(11, 40, "                       ", bg);
        }
    }
    
    if (mouse_in_vis) show_mouse();
}

void draw_playlist() {
    if (ui_view != 3) return;

    static int last_scroll = -1, last_selected = -1, last_current = -1, last_count = -1;
    static int last_hover = -1;
    static int force_redraw = 1;

    if (browser_needs_redraw || ui_view != 3 || queue_scroll != last_scroll || 
        queue_selected != last_selected || queue_current != last_current || queue_count != last_count) {
        force_redraw = 1;
    }

    int current_hover = 0;
    if (setting_hover_effects) {
        if (mouse_y >= 5 && mouse_y <= 15 && mouse_x >= 1 && mouse_x <= 76) {
            int hover_idx = queue_scroll + (mouse_y - 5);
            if (hover_idx < queue_count) current_hover = 500 + hover_idx;
        } else if (mouse_y == 17) {
            if (mouse_x >= 1 && mouse_x <= 19) current_hover = 410; // Save
            else if (mouse_x >= 21 && mouse_x <= 34) current_hover = 411; // Add
            else if (mouse_x >= 36 && mouse_x <= 48) current_hover = 412; // Clear
            else if (mouse_x >= 50 && mouse_x <= 60) current_hover = 413; // Move Down
            else if (mouse_x >= 62 && mouse_x <= 70) current_hover = 414; // Move Up
            else if (mouse_x >= 72 && mouse_x <= 76) current_hover = 415; // Del
            else if (mouse_x == 78) current_hover = 417; // Down Arrow
        } else if (mouse_y == 3) {
            if (mouse_x == 78) current_hover = 416; // Up Arrow
        }
    }

    if (!force_redraw && current_hover == last_hover) return;

    int full_redraw = force_redraw;
    force_redraw = 0; browser_needs_redraw = 0;

    unsigned char bg = get_bg_color(), hl = get_hl_color(), ac = get_accent_color(), cr = get_cr_color();

    if (has_mouse) hide_mouse();

    if (full_redraw) {
        // Clear main rect
        for (int r = 3; r <= 17; r++) {
            for (int c = 1; c < 79; c++) set_char(r, c, ' ', bg);
        }
        
        // Row 2: Replace standard player line to connect UI up to tabs
        set_char(2, 0, 0xC3, bg); for(int i=1; i<79; i++) set_char(2, i, 0xC4, bg);
        set_char(2, 7, 0xC1, bg); set_char(2, 15, 0xC1, bg); set_char(2, 24, 0xC1, bg); set_char(2, 75, 0xC1, bg);
        set_char(2, 77, 0xC2, bg); set_char(2, 79, 0xB4, bg);
        
        // Row 3: Header, Track Counter & Up Arrow
        print_text(3, 2, " Playlist View", bg);
        char trk_buf[30]; sprintf(trk_buf, "Total Tracks: %-4d", queue_count);
        print_text(3, 55, trk_buf, bg);
        set_char(3, 77, 0xB3, bg);
        
        // Row 4: Separator
        set_char(4, 0, 0xC3, bg); for(int i=1; i<77; i++) set_char(4, i, 0xC4, bg);
        set_char(4, 77, 0xC5, bg); set_char(4, 78, 0xC4, bg); set_char(4, 79, 0xB4, bg);

        // Right vertical separator for rows 5-15
        for (int r = 5; r <= 15; r++) set_char(r, 77, 0xB3, bg);
        
        // Row 16: Separator above new buttons
        set_char(16, 0, 0xC3, bg); for(int i=1; i<79; i++) set_char(16, i, 0xC4, bg);
        set_char(16, 20, 0xC2, bg); set_char(16, 35, 0xC2, bg); set_char(16, 49, 0xC2, bg);
        set_char(16, 61, 0xC2, bg); set_char(16, 71, 0xC2, bg); set_char(16, 77, 0xC5, bg);
        set_char(16, 79, 0xB4, bg);

        // Row 18: Re-draw separator to link buttons to progress bar perfectly
        set_char(18, 0, 0xC3, bg); for(int i=1; i<79; i++) set_char(18, i, 0xC4, bg);
        set_char(18, 8, 0xC2, bg);  // Prog Bar Left
        set_char(18, 20, 0xC1, bg); // Save Right
        set_char(18, 35, 0xC1, bg); // Add Right
        set_char(18, 49, 0xC1, bg); // Clear Right
        set_char(18, 61, 0xC1, bg); // Down Right
        set_char(18, 71, 0xC5, bg); // Up Right & Prog bar Right
        set_char(18, 77, 0xC1, bg); // Del Right
        set_char(18, 79, 0xB4, bg);
    }

    if (full_redraw || current_hover == 416 || last_hover == 416) {
        set_char(3, 78, 30, (current_hover == 416) ? hl : bg);
    }

    // Render Queue Items (Now looping to 11!)
    if (full_redraw || current_hover >= 500 || last_hover >= 500) {
        for (int i = 0; i < 11; i++) {
            int idx = queue_scroll + i;
            if (idx < queue_count) {
                int is_hovered = (current_hover == 500 + idx);
                int is_selected = (idx == queue_selected);
                int is_playing = (idx == queue_current);

                unsigned char row_bg = (is_hovered || is_selected) ? 0x10 : (bg & 0xF0);
                unsigned char row_fg = (bg & 0x0F); 
                
                if (is_playing) row_fg = 0x0A;      
                else if (is_selected) row_fg = 0x0B;
                else if (is_hovered) row_fg = 0x0F; 
                
                unsigned char c_col = row_bg | row_fg;

                char row_buf[80], num_buf[5], disp_name[45], status[25] = "";
                sprintf(num_buf, "%2d", idx + 1);
                
                if (strlen(queue_displays[idx]) > 42) {
                    strncpy(disp_name, queue_displays[idx], 39); disp_name[39] = '\0'; strcat(disp_name, "...");
                } else strcpy(disp_name, queue_displays[idx]);
                
                if (is_playing) strcpy(status, "\x1B Currently Playing"); 
                else if (idx == queue_current + 1) strcpy(status, "  Next");

                sprintf(row_buf, " %s %-42s %-22s", num_buf, disp_name, status);
                print_text(5 + i, 1, row_buf, c_col);
            } else {
                print_text(5 + i, 1, "                                                                            ", bg);
            }
        }
        
        // Draw Scroll Thumb (Now scales perfectly from row 5 to 15)
        if (queue_count > 11) {
            int thumb_h = 121 / queue_count; if (thumb_h < 1) thumb_h = 1;
            int thumb_pos = (queue_scroll * (11 - thumb_h)) / (queue_count - 11);
            for (int r = 5; r <= 15; r++) {
                if (r - 5 >= thumb_pos && r - 5 < thumb_pos + thumb_h) set_char(r, 78, 219, bg);
                else set_char(r, 78, 0xB0, 0x08);
            }
        } else {
            for (int r = 5; r <= 15; r++) set_char(r, 78, 0xB0, 0x08);
        }
    }

    // Render Action Buttons
    if (full_redraw || current_hover >= 410 || last_hover >= 410) {
        unsigned char b1 = (current_hover == 410) ? hl : bg; unsigned char b2 = (current_hover == 411) ? hl : bg;
        unsigned char b3 = (current_hover == 412) ? hl : bg; unsigned char b4 = (current_hover == 413) ? hl : bg;
        unsigned char b5 = (current_hover == 414) ? hl : bg; unsigned char b6 = (current_hover == 415) ? cr : bg;

        print_text(17, 1, " Save queue as m3u ", b1); set_char(17, 20, 0xB3, bg);
        print_text(17, 21, " Add to queue ", b2); set_char(17, 35, 0xB3, bg);
        print_text(17, 36, " Clear Queue ", b3); set_char(17, 49, 0xB3, bg);
        print_text(17, 50, " Move Down ", b4); set_char(17, 61, 0xB3, bg);
        print_text(17, 62, " Move Up ", b5); set_char(17, 71, 0xB3, bg);
        print_text(17, 72, " Del ", b6); set_char(17, 77, 0xB3, bg);
        set_char(17, 78, 31, (current_hover == 417) ? hl : bg); // Down arrow
    }



    last_scroll = queue_scroll; last_selected = queue_selected; last_current = queue_current; last_count = queue_count; last_hover = current_hover;
    if (has_mouse) show_mouse();
}

void draw_media_info() {
    if (ui_view != 4) return;
    
    static int last_hover = -1;
    static int last_scroll = -1;
    static int force_redraw = 1;

    // Track if hover state changed or scroll moved
    if (browser_needs_redraw || force_ui_redraw || info_scroll != last_scroll) {
        force_redraw = 1;
    }

    int current_hover = 0;
    if (setting_hover_effects) {
        // Detect hover over the Back button
        if (mouse_y == 3 && mouse_x >= 69 && mouse_x <= 76) current_hover = 1; 
    }

    if (!force_redraw && current_hover == last_hover) return;

    int full_redraw = force_redraw;
    force_redraw = 0; browser_needs_redraw = 0; force_ui_redraw = 0;

    unsigned char bg = get_bg_color(), hl = get_hl_color();
    if (has_mouse) hide_mouse();
    
    if (full_redraw || current_hover != last_hover) {
        
        if (full_redraw) {
            for (int r = 3; r <= 17; r++) { for (int c = 1; c < 79; c++) set_char(r, c, ' ', bg); }
            set_char(2, 0, 0xC3, bg); for(int i=1; i<79; i++) set_char(2, i, 0xC4, bg);
            set_char(2, 7, 0xC1, bg); set_char(2, 15, 0xC1, bg); set_char(2, 24, 0xC1, bg); set_char(2, 75, 0xC1, bg); set_char(2, 77, 0xC2, bg); set_char(2, 79, 0xB4, bg);
            
            for (int r = 3; r <= 17; r++) set_char(r, 77, 0xB3, bg);
            set_char(3, 78, 30, bg); set_char(17, 78, 31, bg); 
            
            char info_lines[40][80];
            int line_count = 0;
            
            sprintf(info_lines[line_count++], " --- Media Info ---");
            if (!has_active_file) {
                sprintf(info_lines[line_count++], " File Type:     No File");
            } else if (file_is_native_wav) {
                sprintf(info_lines[line_count++], " File Type:     WAV (Native PCM)");
            } else {
                sprintf(info_lines[line_count++], " File Type:     %s", global_is_486 ? "WAV (Transcoded Cache)" : "MP3 (MPEG Audio)");
            }
            const char* b_name = get_basename(app_filename);
            int name_len = strlen(b_name);
            
            if (name_len <= 60) {
                sprintf(info_lines[line_count++], " File Name:     %s", b_name);
            } else {
                // Find a clean place to break (space or hyphen) for the first line
                int break1 = 60;
                while (break1 > 40 && b_name[break1] != ' ' && b_name[break1] != '-') break1--;
                if (break1 == 40) break1 = 60; // Failsafe: Hard break if it's one giant word
                
                char line1[65] = {0}; 
                strncpy(line1, b_name, break1);
                sprintf(info_lines[line_count++], " File Name:     %s", line1);
                
                int start2 = break1; 
                if (b_name[start2] == ' ') start2++; // Skip the space so it doesn't indent weirdly
                
                if (name_len - start2 <= 60) {
                    sprintf(info_lines[line_count++], "                %s", b_name + start2);
                } else {
                    // Find a clean break for the second line if it's exceptionally long
                    int break2 = 60;
                    while (break2 > 40 && b_name[start2 + break2] != ' ' && b_name[start2 + break2] != '-') break2--;
                    if (break2 == 40) break2 = 60;
                    
                    char line2[65] = {0}; 
                    strncpy(line2, b_name + start2, break2);
                    sprintf(info_lines[line_count++], "                %s", line2);
                    
                    int start3 = start2 + break2; 
                    if (b_name[start3] == ' ') start3++;
                    sprintf(info_lines[line_count++], "                %.60s", b_name + start3);
                }
            }
            sprintf(info_lines[line_count++], " File Size:     %.2f MB", (float)file_size / 1048576.0f);
            sprintf(info_lines[line_count++], " Audio Bitrate: %d kbps", file_bitrate_kbps);
            sprintf(info_lines[line_count++], " Bitrate Type:  %s", file_is_vbr ? "Variable (VBR)" : "Constant (CBR)");
            sprintf(info_lines[line_count++], " Sample Rate:   %d Hz", current_sample_rate);
            sprintf(info_lines[line_count++], " Length:        %02d:%02d", ui_total_seconds / 60, ui_total_seconds % 60);
            sprintf(info_lines[line_count++], " ");
            sprintf(info_lines[line_count++], " --- Id3 Information ---");
            sprintf(info_lines[line_count++], " Artist:        %s", strlen(id3_artist) > 0 ? id3_artist : "Unknown");
            sprintf(info_lines[line_count++], " Title:         %s", strlen(id3_title) > 0 ? id3_title : "Unknown");
            sprintf(info_lines[line_count++], " Remix/Interp:  %s", strlen(id3_remix) > 0 ? id3_remix : "Unknown");
            sprintf(info_lines[line_count++], " Subtitle/Mix:  %s", strlen(id3_subtitle) > 0 ? id3_subtitle : "Unknown");
            sprintf(info_lines[line_count++], " Album:         %s", strlen(id3_album) > 0 ? id3_album : "Unknown");
            sprintf(info_lines[line_count++], " Album Artist:  %s", strlen(id3_album_artist) > 0 ? id3_album_artist : "Unknown");
            sprintf(info_lines[line_count++], " Year:          %s", strlen(id3_year) > 0 ? id3_year : "Unknown");
            sprintf(info_lines[line_count++], " Track Num:     %s", strlen(id3_track) > 0 ? id3_track : "Unknown");
            sprintf(info_lines[line_count++], " Genre:         %s", strlen(id3_genre) > 0 ? id3_genre : "Unknown");
            sprintf(info_lines[line_count++], " BPM:           %s", strlen(id3_bpm) > 0 ? id3_bpm : "Unknown");
            sprintf(info_lines[line_count++], " Initial Key:   %s", strlen(id3_key) > 0 ? id3_key : "Unknown");
            sprintf(info_lines[line_count++], " Composer:      %s", strlen(id3_composer) > 0 ? id3_composer : "Unknown");
            sprintf(info_lines[line_count++], " Publisher:     %s", strlen(id3_label) > 0 ? id3_label : "Unknown");
            sprintf(info_lines[line_count++], " Encoded By:    %s", strlen(id3_encoded_by) > 0 ? id3_encoded_by : "Unknown");
            sprintf(info_lines[line_count++], " Copyright:     %s", strlen(id3_copyright) > 0 ? id3_copyright : "Unknown");
            
            int visible_lines = 15;
            
            // Failsafe clamp to prevent scrolling past the data!
            int max_scroll = line_count - visible_lines; if (max_scroll < 0) max_scroll = 0;
            if (info_scroll > max_scroll) info_scroll = max_scroll;
            if (info_scroll < 0) info_scroll = 0;

            for (int i = 0; i < visible_lines; i++) {
                int idx = info_scroll + i;
                if (idx < line_count) {
                    char padded_line[80];
                    // --- ADD .76 TO THE FORMATTER HERE! ---
                    sprintf(padded_line, "%-76.76s", info_lines[idx]); 
                    // --------------------------------------
                    print_text(3 + i, 1, padded_line, bg);
                }
            }
            
            // Scrollbar thumb logic
            int thumb_h = (visible_lines * visible_lines) / line_count; if (thumb_h < 1) thumb_h = 1;
            int thumb_pos = max_scroll > 0 ? (info_scroll * (13 - thumb_h)) / max_scroll : 0;
            for (int r = 4; r <= 16; r++) {
                if (max_scroll > 0 && r - 4 >= thumb_pos && r - 4 < thumb_pos + thumb_h) set_char(r, 78, 219, bg);
                else set_char(r, 78, 0xB0, 0x08);
            }
            
            set_char(18, 0, 0xC3, bg); for(int i=1; i<79; i++) set_char(18, i, 0xC4, bg); set_char(18, 8, 0xC2, bg); set_char(18, 71, 0xC2, bg); set_char(18, 77, 0xC1, bg); set_char(18, 79, 0xB4, bg);
        }

        // Draw Back Button (Drawn AFTER the text so it cleanly overwrites that corner!)
        unsigned char btn_col = (current_hover == 1) ? hl : bg;
        print_mapped_str(3, 70, "6 Back ", btn_col);
        print_mapped_str(2, 69, "^", bg); // T
        set_char(3, 69, 0xB3, bg); // │
        set_char(4, 69, 0xC0, bg); // └
        for(int i=70; i<=76; i++) set_char(4, i, 0xC4, bg); // ─
        set_char(4, 77, 0xB4, bg); // ┤
    }
    
    last_hover = current_hover;
    last_scroll = info_scroll;
    if (has_mouse) show_mouse();
}

void draw_file_browser() {
    if (ui_view != 1) return;

    static int last_scroll = -1, last_selected = -1;
    static int last_hover = -1;
    static int last_view = -1;
    static int force_redraw = 1;
    static int last_input_field = 0;
    static int last_tab_elem = -1;

    if (browser_needs_redraw || ui_view != last_view || 
        file_scroll != last_scroll || file_selected != last_selected ||
        active_input_field != last_input_field ||
        active_tab_element != last_tab_elem) { 
        force_redraw = 1;
    }
    last_input_field = active_input_field;
    last_tab_elem = active_tab_element;

    // 2. Build the Target Map for Hover Elements
    int current_hover = 0;
    if (setting_hover_effects) {
        if (mouse_y == 1){
            if(mouse_x >= 76 && mouse_x <= 78) current_hover = 1; // Top X
            else if(mouse_x >=0 && mouse_x <=75) current_hover = 17; 
            else if(mouse_x == 79) current_hover = 17; 
        } 
        else if (mouse_y == 0 || mouse_y == 2 || mouse_y == 3 || mouse_y == 5 || mouse_y == 6 || mouse_y == 8 || mouse_y == 9 || mouse_y == 10 || mouse_y == 12 || mouse_y == 13 || mouse_y == 16 || mouse_y == 21 || mouse_y == 19 || mouse_y == 22 || mouse_y == 23 || mouse_y == 24) 
            current_hover = 17; 

        else if (mouse_y == 20){
            if (mouse_x >= 62 && mouse_x <= 66) current_hover = 2; // OK Button
            else if (mouse_x <= 62 && mouse_x >= 0) current_hover = 17; // Hover on empty space to left of OK
            else if (mouse_x >= 71 && mouse_x <= 76) current_hover = 3; //cancel Button
            else if (mouse_x >= 77 && mouse_x <= 79) current_hover = 17;
            else if (mouse_x >= 68 && mouse_x <= 70) current_hover = 17;
        }
        else if (mouse_y == 7 ) {
            if (mouse_x == 76) current_hover = 4; // Scroll Up
            else current_hover = 17; 
        }
        else if (mouse_y == 17 ) {
            if (mouse_x == 76) current_hover = 5; // Scroll down
            else current_hover = 17; 
        }
        else if (mouse_y == 4) {
            if (mouse_x >= 3 && mouse_x <= 4) current_hover = 6; // Nav Prev
            else if (mouse_x >= 8 && mouse_x <= 9) current_hover = 7; // Nav Next
            else if (mouse_x == 13) current_hover = 8; // Nav Up
            else current_hover = 17; 
        }
    }
    
    if (!force_redraw && current_hover == last_hover) {
        return; 
    }

    int full_redraw = force_redraw;
    force_redraw = 0;
    browser_needs_redraw = 0;
    last_view = ui_view;
    last_scroll = file_scroll; 
    last_selected = file_selected;
    
    if (has_mouse) hide_mouse();

    unsigned char bg_color = get_bg_color(), hl_color = get_hl_color(), cr_color = get_cr_color(); 
    unsigned char dim_color = get_dim_color();
    char sdisp[20], statdisp[70]; 
    int lsize, th, tsize, tpos; 

    // 4. The Heavy Paint: Draw the massive layout only when needed
    if (full_redraw) {
        const char* fd[25] = {
            "[******^*******^********^**************************************************^***]", 
            "| File | Audio | Visual |                                                  | X |", 
            "@******_*******_********_**************************************************_***$", 
            "| [**] [**] [*] [*******************************************] [**************] |", 
            "| |4*| |*5| |1| |                                           | |              | |", 
            "| {**} {**} {*} {*******************************************} {**************} |", 
            "| [***********] [**********************************************************^*] |", 
            "| |  jump to  | | Title                                |  type  |   size   |1| |", 
            "| @***********$ @**********************************************************+*$ |", 
            "| |  C:       | |                                                          |?| |", 
            "| |  D:       | |                                                          | | |", 
            "| |  E:       | |                                                          | | |", 
            "| |  F:       | |                                                          | | |", 
            "| |  G:       | |                                                          | | |", 
            "| |  H:       | |                                                          | | |", 
            "| |           | |                                                          | | |", 
            "| |           | |                                                          | | |", 
            "| |           | |                                                          |2| |", 
            "| {***********} {**********************************************************_*} |", 
            "|               [******************************************] [******] [******] |", 
            "|   File Name:  |                                          | |  ok  | |cancel| |", 
            "|               {******************************************} {******} {******} |", 
            "@***********^******************************************************************$", 
            "| Open File |                                                                  |", 
            "{***********_******************************************************************}"
        };
        
        for (int r = 0; r < 25; r++) print_mapped_str(r, 0, fd[r], bg_color);
        
        // --- NEW SAVE AS TEXT ---
        if (browser_save_mode) {
            print_text(23, 2, " Save As ", bg_color);
        } else {
            print_text(23, 2, "Open File", bg_color);
        }
        // ------------------------

        char title_buf[50];
        sprintf(title_buf, "%.30s", get_basename(app_filename));
        print_text(1, 26, title_buf, bg_color);
        
        // --- DYNAMIC ADDRESS BAR EDITING & SCROLLING ---
        int draw_addr_len = strlen(dialog_address_text);
        int addr_offset = 0;
        char pdisp[45];
        memset(pdisp, ' ', 41); pdisp[41] = '\0';
        
        if (active_input_field != 3 && draw_addr_len > 41) {
            strncpy(pdisp, dialog_address_text, 38);
            pdisp[38] = '.'; pdisp[39] = '.'; pdisp[40] = '.';
        } else {
            if (dialog_address_cursor > 38) addr_offset = dialog_address_cursor - 38;
            for(int k=0; k<41 && (addr_offset + k) < draw_addr_len; k++) {
                pdisp[k] = dialog_address_text[addr_offset + k];
            }
        }

        print_text(4, 18, pdisp, bg_color);
        if (active_input_field == 3) {
            int cx = 18 + dialog_address_cursor - addr_offset;
            if (cx > 58) cx = 58;
            set_char(4, cx, 221, bg_color); // Address cursor █
        }
        
        // Drive letters are now completely static (no hover states!)
        print_text(9, 4, " C: ", bg_color);
        print_text(10, 4, " D: ", bg_color);
        print_text(11, 4, " E: ", bg_color);
        print_text(12, 4, " F: ", bg_color);
        print_text(13, 4, " G: ", bg_color);
        print_text(14, 4, " H: ", bg_color);
        
        // --- DYNAMIC FILE NAME EDITING & SCROLLING ---
        int draw_fn_len = strlen(dialog_file_name);
        int fn_offset = 0;
        char idisp[45];
        memset(idisp, ' ', 40); idisp[40] = '\0';
        
        if (active_input_field != 4 && draw_fn_len > 40) {
            strncpy(idisp, dialog_file_name, 37);
            idisp[37] = '.'; idisp[38] = '.'; idisp[39] = '.';
        } else {
            if (dialog_filename_cursor > 38) fn_offset = dialog_filename_cursor - 38;
            for(int k=0; k<40 && (fn_offset + k) < draw_fn_len; k++) {
                idisp[k] = dialog_file_name[fn_offset + k];
            }
        }
        print_text(20, 18, idisp, bg_color);
        if (active_input_field == 4) {
            int cx = 18 + dialog_filename_cursor - fn_offset;
            if (cx > 57) cx = 57;
            set_char(20, cx, 221, bg_color); // File name cursor █
        }
        
        if (strlen(browser_status_msg) > 0) {
            strcpy(statdisp, browser_status_msg);
        } else {
            sprintf(statdisp, "Found %d items in directory.", file_count);
        }
        while(strlen(statdisp) < 62) strcat(statdisp, " "); 
        print_text(23, 14, statdisp, bg_color);
        
        // Scrollbar Track Background
        lsize = file_count; th = 8; 
        tsize = lsize > 9 ? (int)((8L * 9) / lsize) : th; if (tsize < 1) tsize = 1; 
        tpos = lsize > 9 ? (int)((long)file_scroll * (8 - tsize) / (lsize - 9)) : 0;
        
        for (int i = 0; i < th; i++) {
            set_char(9 + i, 76, (lsize <= 9 || (i >= tpos && i < tpos + tsize)) ? 219 : 0xB0, (lsize <= 9 || (i >= tpos && i < tpos + tsize)) ? bg_color : dim_color);
        }
    }

    // 5. Selective Paint: The Buttons using Your Dual Logic!
    
    // Removed dim_color to ensure Mode 7 compatibility! Disabled buttons simply stay bg_color.
    unsigned char prev_col_h = (path_history_index > 0) ? hl_color : bg_color;
    unsigned char prev_col_n = (active_tab_element == 7) ? hl_color : bg_color;
    
    unsigned char next_col_h = (path_history_index < path_history_count - 1) ? hl_color : bg_color;
    unsigned char next_col_n = (active_tab_element == 8) ? hl_color : bg_color;
    
    unsigned char up_col_h = (strlen(current_dir) > 3) ? hl_color : bg_color;
    unsigned char up_col_n = (active_tab_element == 9) ? hl_color : bg_color;

    if(!is_paused){
        if (full_redraw || current_hover == 1 || last_hover != 1 || active_tab_element == 10 || last_tab_elem == 10) print_text(1, 76, " X ", (current_hover == 1 || active_tab_element == 10) ? cr_color : bg_color);
        if (full_redraw || current_hover == 2 || last_hover != 2 || active_tab_element == 5 || last_tab_elem == 5) print_text(20, 62, "  ok  ", (current_hover == 2 || active_tab_element == 5) ? hl_color : bg_color);
        if (full_redraw || current_hover == 3 || last_hover != 3 || active_tab_element == 6 || last_tab_elem == 6) print_text(20, 71, "cancel", (current_hover == 3 || active_tab_element == 6) ? cr_color : bg_color);
        if (full_redraw || current_hover == 4 || last_hover != 4) set_char(7, 76, decode_char('1'), (current_hover == 4) ? hl_color : bg_color);
        if (full_redraw || current_hover == 5 || last_hover != 5) set_char(17, 76, decode_char('2'), (current_hover == 5) ? hl_color : bg_color);
        if (full_redraw || current_hover == 6 || last_hover != 6 || active_tab_element == 7 || last_tab_elem == 7) print_mapped_str(4, 3, "4*", (current_hover == 6) ? prev_col_h : prev_col_n);
        if (full_redraw || current_hover == 7 || last_hover != 7 || active_tab_element == 8 || last_tab_elem == 8) print_mapped_str(4, 8, "*5", (current_hover == 7) ? next_col_h : next_col_n);
        if (full_redraw || current_hover == 8 || last_hover != 8 || active_tab_element == 9 || last_tab_elem == 9) print_mapped_str(4, 13, "1", (current_hover == 8) ? up_col_h : up_col_n);
    }
    else if(is_paused){
        if (full_redraw || current_hover == 1 || last_hover == 1 || active_tab_element == 10 || last_tab_elem == 10) print_text(1, 76, " X ", (current_hover == 1 || active_tab_element == 10) ? cr_color : bg_color);
        if (full_redraw || current_hover == 2 || last_hover == 2 || active_tab_element == 5 || last_tab_elem == 5) print_text(20, 62, "  ok  ", (current_hover == 2 || active_tab_element == 5) ? hl_color : bg_color);
        if (full_redraw || current_hover == 3 || last_hover == 3 || active_tab_element == 6 || last_tab_elem == 6) print_text(20, 71, "cancel", (current_hover == 3 || active_tab_element == 6) ? cr_color : bg_color);
        if (full_redraw || current_hover == 4 || last_hover == 4) set_char(7, 76, decode_char('1'), (current_hover == 4) ? hl_color : bg_color);
        if (full_redraw || current_hover == 5 || last_hover == 5) set_char(17, 76, decode_char('2'), (current_hover == 5) ? hl_color : bg_color);
        if (full_redraw || current_hover == 6 || last_hover == 6 || active_tab_element == 7 || last_tab_elem == 7) print_mapped_str(4, 3, "4*", (current_hover == 6) ? prev_col_h : prev_col_n);
        if (full_redraw || current_hover == 7 || last_hover == 7 || active_tab_element == 8 || last_tab_elem == 8) print_mapped_str(4, 8, "*5", (current_hover == 7) ? next_col_h : next_col_n);
        if (full_redraw || current_hover == 8 || last_hover == 8 || active_tab_element == 9 || last_tab_elem == 9) print_mapped_str(4, 13, "1", (current_hover == 8) ? up_col_h : up_col_n);
    }
    
    // 5.5 Paint: The Dynamic Search Box!
    if (full_redraw) {
        if (strlen(dialog_search_text) == 0 && active_input_field != 2) { 
            strcpy(sdisp, "search here "); 
            while(strlen(sdisp) < 12) strcat(sdisp, " "); 
        } else { 
            if (strlen(dialog_search_text) > 12) { 
                strcpy(sdisp, dialog_search_text + (strlen(dialog_search_text) - 12)); 
            } else { 
                strcpy(sdisp, dialog_search_text); 
                while(strlen(sdisp) < 12) strcat(sdisp, " "); 
            } 
        } 
        
        print_text(4, 64, sdisp, bg_color);
        if (active_input_field == 2) {
            int cx = 64 + dialog_search_cursor;
            if (cx > 75) cx = 75;
            set_char(4, cx, 221, bg_color);  //search cursor
        }
    }

    // 6. Paint: The File List!
    if (full_redraw) {
        lsize = file_count;
        for (int i = 0; i < 9; i++) {
            if (file_scroll + i < lsize) {
                int idx = file_scroll + i; 
                
                unsigned char c_col = (idx == file_selected && active_tab_element == 3) ? hl_color : bg_color;
                
                char n_buf[45], s_buf[15], row_buf[80];
                if (strlen(file_list[idx].name) > 36) { 
                    strncpy(n_buf, file_list[idx].name, 33); n_buf[33] = '\0'; strcat(n_buf, "..."); 
                } else { 
                    strcpy(n_buf, file_list[idx].name); while(strlen(n_buf) < 36) strcat(n_buf, " "); 
                }
                
                if (file_list[idx].is_dir) { 
                    strcpy(s_buf, "     "); 
                } else { 
                    long kb = file_list[idx].size / 1024; if (kb == 0 && file_list[idx].size > 0) kb = 1; 
                    sprintf(s_buf, "%4ldKB", kb); while(strlen(s_buf) < 6) strcat(s_buf, " "); 
                }
                
                const char* type_str = file_list[idx].is_dir ? "Folder" : ((ends_with_ignore_case(file_list[idx].name, ".m3u") || ends_with_ignore_case(file_list[idx].name, ".m3u8")) ? "P.List" : "Audio ");
                sprintf(row_buf, "%s \xB3 %s \xB3 %s", n_buf, type_str, s_buf); 
                print_text(9 + i, 18, row_buf, c_col);
            } else {
                print_text(9 + i, 18, "                                                         ", bg_color);
            }
        }
    }
    
    last_hover = current_hover;
    if (has_mouse) show_mouse();
}

void draw_settings() {
    if (ui_view != 2) return;
    
    static int last_tab = -1;
    static int last_hover = -1;
    static int last_ansi = -1;
    static int last_he = -1;
    static int last_color = -1;
    static int last_tab_elem = -1;
    static int force_redraw = 1;

    if (browser_needs_redraw || current_settings_tab != last_tab || setting_ansi_mode != last_ansi) {
        force_redraw = 1;
    }

    int current_hover = 0; 
    
    if (setting_hover_effects) {
        if (mouse_y == 1 && mouse_x >= 77 && mouse_x <= 79) current_hover = 1; // Top X
        else if (mouse_y == 3) {
            if (mouse_x >= 17 && mouse_x <= 23) current_hover = 11; // Audio Tab
            else if (mouse_x >= 25 && mouse_x <= 33) current_hover = 2; // Display Tab
            else if (mouse_x >= 35 && mouse_x <= 44) current_hover = 3; // Graphics Tab
            else if (mouse_x >= 46 && mouse_x <= 52) current_hover = 4; // Other Tab
            else if (mouse_x >= 54 && mouse_x <= 61) current_hover = 12; // Visual Tab
            else if (mouse_x >= 71 && mouse_x <= 78) current_hover = 5; // Back
        }
        else if (current_settings_tab == 1) { // Graphics
            if (mouse_y == 7 && mouse_x >= 4 && mouse_x <= 10) current_hover = 6; 
            else if (mouse_y == 8 && mouse_x >= 4 && mouse_x <= 10) current_hover = 7;
            else if (mouse_y == 11 && mouse_x >= 2 && mouse_x <= 20) current_hover = 8;
            else if (mouse_y == 14 && mouse_x >= 4 && mouse_x <= 25) current_hover = 9;
            else if (mouse_y == 15 && mouse_x >= 4 && mouse_x <= 26) current_hover = 10;
            else if (mouse_y == 18 && mouse_x >= 26 && mouse_x <= 77) current_hover = 18;
            else if (mouse_y == 19 && mouse_x >= 26 && mouse_x <= 77) current_hover = 19;
            else if (setting_color && mouse_y == 20 && mouse_x >= 26 && mouse_x <= 77) current_hover = 20;
            else if (setting_color && mouse_y == 21 && mouse_x >= 26 && mouse_x <= 77) current_hover = 21;
        }
        else if (current_settings_tab == 3) { // Audio
            if (mouse_y == 8) {
                if (mouse_x >= 26 && mouse_x <= 42) current_hover = 300; 
                else if (mouse_x >= 51 && mouse_x <= 68) current_hover = 301; 
            } else if (mouse_y == 10) {
                if (mouse_x >= 26 && mouse_x <= 39) current_hover = 302; 
                else if (mouse_x >= 42 && mouse_x <= 48) current_hover = 303; 
                else if (mouse_x >= 51 && mouse_x <= 57) current_hover = 304; 
                else if (mouse_x >= 61 && mouse_x <= 66) current_hover = 305; 
            } else if (mouse_y == 12) {
                if (mouse_x >= 26 && mouse_x <= 40) current_hover = 306; 
                else if (mouse_x >= 51 && mouse_x <= 65) current_hover = 307; 
            } else if (mouse_y == 14) {
                if (mouse_x >= 26 && mouse_x <= 38) current_hover = 308; 
                else if (mouse_x >= 42 && mouse_x <= 49) current_hover = 309; 
                else if (mouse_x >= 51 && mouse_x <= 65) current_hover = 310; 
            } else if (mouse_y == 16) {
                if (mouse_x >= 26 && mouse_x <= 31) current_hover = 311; 
                else if (mouse_x >= 33 && mouse_x <= 40) current_hover = 312; 
                else if (mouse_x >= 42 && mouse_x <= 47) current_hover = 313; 
                else if (mouse_x >= 51 && mouse_x <= 56) current_hover = 314; 
            } else if (mouse_y == 18) {
                if (mouse_x >= 26 && mouse_x <= 36) current_hover = 315; 
                else if (mouse_x >= 51 && mouse_x <= 62) current_hover = 316; 
            } else if (mouse_y == 22) {
                if (mouse_x >= 62 && mouse_x <= 76) current_hover = 317; 
            }
        }
        else if (current_settings_tab == 4) { // Visualizer
            if (mouse_y == 7) {
                if (mouse_x >= 4 && mouse_x <= 13) current_hover = 400; 
                else if (mouse_x >= 32 && mouse_x <= 39) current_hover = 401; 
                else if (mouse_x >= 41 && mouse_x <= 50) current_hover = 402; 
                else if (mouse_x >= 52 && mouse_x <= 59) current_hover = 403; 
            } else if (mouse_y == 8) {
                if (mouse_x >= 17 && mouse_x <= 19) current_hover = 404; 
                else if (mouse_x >= 23 && mouse_x <= 25) current_hover = 405; 
                else if (mouse_x >= 40 && mouse_x <= 42) current_hover = 406; 
                else if (mouse_x >= 46 && mouse_x <= 48) current_hover = 407; 
                else if (mouse_x >= 63 && mouse_x <= 65) current_hover = 408; 
                else if (mouse_x >= 69 && mouse_x <= 71) current_hover = 409; 
            } else if (mouse_y == 11) {
                if (mouse_x >= 4 && mouse_x <= 13) current_hover = 410; 
                else if (mouse_x >= 32 && mouse_x <= 39) current_hover = 411; 
                else if (mouse_x >= 41 && mouse_x <= 50) current_hover = 412; 
                else if (mouse_x >= 52 && mouse_x <= 59) current_hover = 413; 
            } else if (mouse_y == 12) {
                if (mouse_x >= 17 && mouse_x <= 19) current_hover = 414; 
                else if (mouse_x >= 23 && mouse_x <= 25) current_hover = 415; 
                else if (mouse_x >= 40 && mouse_x <= 42) current_hover = 416; 
                else if (mouse_x >= 46 && mouse_x <= 48) current_hover = 417; 
                else if (mouse_x >= 63 && mouse_x <= 65) current_hover = 418; 
                else if (mouse_x >= 69 && mouse_x <= 71) current_hover = 419; 
            } else if (mouse_y == 13) {
                if (mouse_x >= 17 && mouse_x <= 25) current_hover = 420; 
                else if (mouse_x >= 27 && mouse_x <= 36) current_hover = 421; 
            } else if (mouse_y == 16) {
                if (mouse_x >= 17 && mouse_x <= 24) current_hover = 422; 
                else if (mouse_x >= 26 && mouse_x <= 34) current_hover = 423; 
                else if (mouse_x >= 36 && mouse_x <= 45) current_hover = 424; 
                else if (mouse_x >= 61 && mouse_x <= 67) current_hover = 425; 
                else if (mouse_x >= 69 && mouse_x <= 75) current_hover = 426; 
            } else if (mouse_y == 18) {
                if (mouse_x >= 18 && mouse_x <= 29) current_hover = 427; 
                else if (mouse_x >= 31 && mouse_x <= 40) current_hover = 428; 
                else if (mouse_x >= 42 && mouse_x <= 51) current_hover = 429; 
            } else if (mouse_y == 22) {
                if (mouse_x >= 62 && mouse_x <= 76) current_hover = 430; 
            }
        }
    }

    if (!force_redraw && current_hover == last_hover &&
        current_settings_tab == last_tab && setting_ansi_mode == last_ansi &&
        setting_hover_effects == last_he && setting_color == last_color && 
        active_tab_element == last_tab_elem) {
        return;
    }

    if (has_mouse) hide_mouse();

    int full_redraw = force_redraw;
    force_redraw = 0; 
    browser_needs_redraw = 0;

    unsigned char bg = get_bg_color(), hl = get_hl_color(), cr = get_cr_color(); 
    char buf[80]; 
    char cb = setting_ansi_mode == 1 ? '\xFB' : 'V'; 
    char rb = setting_ansi_mode == 1 ? '\x07' : '*';

    if (full_redraw) {
        for (int y = 2; y <= 24; y++) {
            print_str(y, 0, "                                                                                ", bg);
        }
        
        print_mapped_str(0, 0, "[******^*******^********^**************************************************^***]", bg); 
        print_mapped_str(1, 0, "| File | Audio | Visual |                                                  | X |", bg); 
        print_mapped_str(2, 0, "@******_***^***_^*******+*********^**********^*******^********^********^***_***$", bg); 
        print_mapped_str(3, 0, "| Settings |    | Audio | Display | Graphics | Other | Visual |        |6 Back |", bg);
        
        for (int y = 5; y <= 23; y++) { 
            set_char(y, 0, decode_char('|'), bg); 
            set_char(y, 79, decode_char('|'), bg); 
        } 
        print_mapped_str(24, 0, "{******************************************************************************}", bg);
    }

    // --- PILLAR 5: GROUPED REDRAW FOR TOP TABS (IDs 1 to 12) ---
    if (full_redraw || (current_hover >= 1 && current_hover <= 12) || (last_hover >= 1 && last_hover <= 12) || active_tab_element != last_tab_elem) {
        print_str(1, 76, " X ", (current_hover == 1) ? cr : bg);
        print_str(3, 17, " Audio ", (current_hover == 11 || current_settings_tab == 3) ? hl : bg); 
        print_str(3, 25, " Display ", (current_hover == 2 || active_tab_element == 40 || current_settings_tab == 0) ? hl : bg); 
        print_str(3, 35, " Graphics ", (current_hover == 3 || active_tab_element == 41 || current_settings_tab == 1) ? hl : bg); 
        print_str(3, 46, " Other ", (current_hover == 4 || active_tab_element == 42 || current_settings_tab == 2) ? hl : bg); 
        print_str(3, 54, " Visual ", (current_hover == 12 || current_settings_tab == 4) ? hl : bg);
        print_mapped_str(3, 72, "6 Back ", (current_hover == 5 || active_tab_element == 43) ? cr : bg);
    }

    if (current_settings_tab == 0) { //display setting
        if (full_redraw) {
            print_mapped_str(4, 0, "@**********_****_*******}         {**********_*******_********_********_*******$", bg); 
            print_str(6, 2, "Mode: 80x25 (More settings available later)", bg);
        }
    }
    else if (current_settings_tab == 1) { //graphics setting
        int disp_bg = setting_color ? setting_bg_color : ((setting_bg_color == 7) ? 7 : 0);
        int disp_fg = setting_color ? setting_fg_color : ((disp_bg == 7) ? 0 : 7);

        if (full_redraw) {
            print_mapped_str(4, 0, "@**********_****_*******_*********}          {*******_********_********_*******$", bg);
            print_str(6, 2, "Hover Effects:", bg); 
            print_str(13, 2, "Ansi vs Unicode compatibility Mode:", bg); 
            print_str(17, 2, "Theme Settings:", bg); 
        }

        // GROUPED REDRAW FOR GRAPHICS (Hover IDs 6-10, 18-21)
        if (full_redraw || (current_hover >= 6 && current_hover <= 10) || (last_hover >= 6 && last_hover <= 10) || 
            (current_hover >= 18 && current_hover <= 21) || (last_hover >= 18 && last_hover <= 21) ||
            setting_hover_effects != last_he || setting_ansi_mode != last_ansi || setting_color != last_color) {
            
            unsigned char he_off_c = (current_hover == 6) ? hl : bg;
            sprintf(buf, "(%c) Off", setting_hover_effects == 0 ? rb : ' '); print_str(7, 4, buf, he_off_c); 
            
            unsigned char he_on_c  = (current_hover == 7) ? hl : bg;
            sprintf(buf, "(%c) On ", setting_hover_effects == 1 ? rb : ' '); print_str(8, 4, buf, he_on_c); 
            
            unsigned char ec_c = (current_hover == 8) ? hl : bg;
            sprintf(buf, "Enable Color [%c]", setting_color ? cb : ' '); print_str(11, 2, buf, ec_c);
            
            unsigned char ansi0_c = (current_hover == 9) ? hl : bg;
            sprintf(buf, "(%c) Standard Ansi Mode", setting_ansi_mode == 0 ? rb : ' '); print_str(14, 4, buf, ansi0_c); 
            
            unsigned char ansi1_c = (current_hover == 10) ? hl : bg;
            sprintf(buf, "(%c) Extended Ascii Mode", setting_ansi_mode == 1 ? rb : ' '); print_str(15, 4, buf, ansi1_c);
            
            print_theme_line(18, "Background Color(BG)", disp_bg, setting_color, bg, hl, mouse_x, mouse_y, setting_hover_effects); 
            print_theme_line(19, "Foreground Color(FG)", disp_fg, setting_color, bg, hl, mouse_x, mouse_y, setting_hover_effects);
            
            if (setting_color) { 
                print_theme_line(20, "Selection  Color(BG)", setting_sel_color, 1, bg, hl, mouse_x, mouse_y, setting_hover_effects); 
                print_theme_line(21, "Critical   Color(BG)", setting_crit_color, 1, bg, hl, mouse_x, mouse_y, setting_hover_effects); 
            } else if (last_color == 1 && setting_color == 0) {
                print_str(20, 2, "                                                                          ", bg);
                print_str(21, 2, "                                                                          ", bg);
            }
        } 
    } 
    else if (current_settings_tab == 2) { //other settings
        if (full_redraw) {
            print_mapped_str(4, 0, "@**********_****_*******_*********_**********}       {********_********_*******$", bg);
            print_str(6, 2, "System:", bg); 
            sprintf(buf, "Mouse Support: %s [%c] (Auto-detected)", use_mouse ? "ON " : "OFF", use_mouse ? cb : ' '); print_str(8, 2, buf, bg);
        }
    } 
    else if (current_settings_tab == 3) { //audio settings
        if (full_redraw) {
            print_mapped_str(4, 0, "@**********_****}       {*********_**********_*******_********_********_*******$", bg);
            print_str(6, 2, "Audio Hardware Engine & Processing:", bg); 
            
            print_str(8, 4, "Output Device       -", bg);
            print_str(10, 4, "Target Sample Rate  -", bg);
            print_str(12, 4, "Output Channels     -", bg);
            print_str(14, 4, "DMA Buffer Size     -", bg);
            print_str(16, 4, "PC Speaker Boost    -", bg);
            print_str(18, 4, "486 Transcode Cache -", bg);
            
            // Hardware Detection Status Bar
            print_mapped_str(20, 1, "******************************************************************************", get_dim_color());
            char hw_buf[80];

            if (settings_saved_flag) {
                sprintf(hw_buf, " Saved, changes will apply on next launch");
            } else if (global_is_pc_speaker) {
                sprintf(hw_buf, " Hardware Detected: Native PC Speaker");
            } else if (dsp_major > 0) {
                sprintf(hw_buf, " Hardware Detected: Sound Blaster %s (DSP v%d.%02d)", is_sb16 ? "16/AWE32" : "Pro / 2.0", dsp_major, dsp_minor);
            } else {
                sprintf(hw_buf, " Hardware Detected: Unknown");
            }
            while(strlen(hw_buf) < 55) strcat(hw_buf, " ");

            print_str(22, 2, hw_buf, get_accent_color());
        }
        //hover logic
        if (full_redraw || (current_hover >= 300 && current_hover <= 317) || (last_hover >= 300 && last_hover <= 317) || browser_needs_redraw) {
            //line1
            sprintf(buf, "(%c) Sound Blaster", !config_is_pc_speaker ? rb : ' ');
            print_str(8, 26, buf, (current_hover == 300) ? hl : bg);
            sprintf(buf, "(%c) PC Speaker PWM", config_is_pc_speaker ? rb : ' ');
            print_str(8, 51, buf, (current_hover == 301) ? hl : bg);

            //line2
            sprintf(buf, "(%c) Auto/44.1k", (custom_sample_rate == 0) ? rb : ' ');
            print_str(10, 26, buf, (current_hover == 302) ? hl : bg);
            sprintf(buf, "(%c) 22k", (custom_sample_rate == 22050) ? rb : ' ');
            print_str(10, 42, buf, (current_hover == 303) ? hl : bg);
            sprintf(buf, "(%c) 11k", (custom_sample_rate == 11025) ? rb : ' ');
            print_str(10, 51, buf, (current_hover == 304) ? hl : bg);
            sprintf(buf, "(%c) 8k", (custom_sample_rate == 8000) ? rb : ' ');
            print_str(10, 61, buf, (current_hover == 305) ? hl : bg);

            //line3
            sprintf(buf, "(%c) Auto/Stereo", (custom_channels == 0 || custom_channels == 2) ? rb : ' '); 
            print_str(12, 26, buf, (current_hover == 306) ? hl : bg);
            sprintf(buf, "(%c) Forced Mono", (custom_channels == 1) ? rb : ' '); 
            print_str(12, 51, buf, (current_hover == 307) ? hl : bg);

            //line4
            sprintf(buf, "(%c) 16KB Fast", (custom_buffer_size == 16384 || (custom_buffer_size == 0 && active_buffer_size == 16384)) ? rb : ' '); 
            print_str(14, 26, buf, (current_hover == 308) ? hl : bg);  
            sprintf(buf, "(%c) 32KB", (custom_buffer_size == 32768 || (custom_buffer_size == 0 && active_buffer_size == 32768)) ? rb : ' '); 
            print_str(14, 42, buf, (current_hover == 309) ? hl : bg);
            sprintf(buf, "(%c) 64KB Stable", (custom_buffer_size == 65536 || (custom_buffer_size == 0 && active_buffer_size == 65536)) ? rb : ' '); 
            print_str(14, 51, buf, (current_hover == 310) ? hl : bg);

            //line5
            sprintf(buf, "(%c) 1X", (pc_speaker_overdrive <= 100) ? rb : ' '); 
            print_str(16, 26, buf, (current_hover == 311) ? hl : bg);
            sprintf(buf, "(%c) 1.5X", (pc_speaker_overdrive == 150) ? rb : ' '); 
            print_str(16, 33, buf, (current_hover == 312) ? hl : bg);
            sprintf(buf, "(%c) 2X", (pc_speaker_overdrive == 200) ? rb : ' '); 
            print_str(16, 42, buf, (current_hover == 313) ? hl : bg);
            sprintf(buf, "(%c) 3X", (pc_speaker_overdrive == 300) ? rb : ' ');
            print_str(16, 51, buf, (current_hover == 314) ? hl : bg);

            //line6
            sprintf(buf, "(%c) Enabled", config_is_486 ? rb : ' '); 
            print_str(18, 26, buf, (current_hover == 315) ? hl : bg);
            sprintf(buf, "(%c) Disabled", !config_is_486 ? rb : ' '); 
            print_str(18, 51, buf, (current_hover == 316) ? hl : bg);

            unsigned char btn_col = (current_hover == 317) ? cr : bg;
            print_mapped_str(21, 61, "[***************]", bg);
            print_mapped_str(22, 61, "|", bg);
            print_mapped_str(22, 62, "  Save Config  ", btn_col);
            print_mapped_str(22, 77, "|", bg);
            print_mapped_str(23, 61, "{***************}", bg);
        }
    }
    else if (current_settings_tab == 4) { //visualizer settings
        if (full_redraw) {
            print_mapped_str(4, 0, "@**********_****_*******_*********_**********_*******}        {********_*******$", bg);
            print_str(6, 2, "Vu Meter:", bg); 
            print_str(10, 2, "Frequency Bars:", bg);
            print_str(15, 2, "Debug Info:", bg);
            
            print_str(7, 17, "Falloff Speed:", bg);
            print_str(8, 4, "Zone 1 Color", bg); print_str(8, 27, "Zone 2 Color", bg); print_str(8, 50, "Zone 3 Color", bg);
            
            print_str(11, 17, "Falloff Speed:", bg);
            print_str(12, 4, "Zone 1 Color", bg); print_str(12, 27, "Zone 2 Color", bg); print_str(12, 50, "Zone 3 Color", bg);
            print_str(13, 4, "Bars Style:", bg);
            
            print_str(16, 4, "Update Rate:", bg); print_str(16, 47, "Detail Level:", bg);
            print_str(18, 2, "Global FPS Cap:", bg);

            print_mapped_str(20, 1, "******************************************************************************", get_dim_color());
            char hw_buf[80];
            if (settings_saved_flag) sprintf(hw_buf, " Saved, changes will apply instantly");
            else sprintf(hw_buf, " Visualizer rendering engine settings");
            while(strlen(hw_buf) < 55) strcat(hw_buf, " ");
            print_str(22, 2, hw_buf, get_accent_color());
        }

        // GROUPED REDRAW FOR VISUAL TAB (Hover IDs 400 - 430)
        if (full_redraw || (current_hover >= 400 && current_hover <= 430) || (last_hover >= 400 && last_hover <= 430) || browser_needs_redraw) {
            // Row 6: VU
            sprintf(buf, "Peaks: [%c]", vis_vu_peaks ? cb : ' '); print_str(7, 4, buf, (current_hover == 400) ? hl : bg);
            sprintf(buf, "Slow (%c)", vis_vu_falloff == 0 ? rb : ' '); print_str(7, 32, buf, (current_hover == 401) ? hl : bg);
            sprintf(buf, "Normal (%c)", vis_vu_falloff == 1 ? rb : ' '); print_str(7, 41, buf, (current_hover == 402) ? hl : bg);
            sprintf(buf, "Fast (%c)", vis_vu_falloff == 2 ? rb : ' '); print_str(7, 52, buf, (current_hover == 403) ? hl : bg);
            
            // Row 8: VU Colors
            print_str(8, 17, "[-]", (current_hover == 404) ? hl : bg); 
            set_char(8, 20, '[', bg); set_char(8, 21, 254, setting_color ? vis_vu_c1 : get_accent_color()); set_char(8, 22, ']', bg); 
            print_str(8, 23, "[+]", (current_hover == 405) ? hl : bg);

            print_str(8, 40, "[-]", (current_hover == 406) ? hl : bg); 
            set_char(8, 43, '[', bg); set_char(8, 44, 254, setting_color ? vis_vu_c2 : get_accent_color()); set_char(8, 45, ']', bg); 
            print_str(8, 46, "[+]", (current_hover == 407) ? hl : bg);

            print_str(8, 63, "[-]", (current_hover == 408) ? hl : bg); 
            set_char(8, 66, '[', bg); set_char(8, 67, 254, setting_color ? vis_vu_c3 : get_accent_color()); set_char(8, 68, ']', bg); 
            print_str(8, 69, "[+]", (current_hover == 409) ? hl : bg);

            // Row 10: Bar
            sprintf(buf, "Peaks: [%c]", vis_bar_peaks ? cb : ' '); print_str(11, 4, buf, (current_hover == 410) ? hl : bg);
            sprintf(buf, "Slow (%c)", vis_bar_falloff == 0 ? rb : ' '); print_str(11, 32, buf, (current_hover == 411) ? hl : bg);
            sprintf(buf, "Normal (%c)", vis_bar_falloff == 1 ? rb : ' '); print_str(11, 41, buf, (current_hover == 412) ? hl : bg);
            sprintf(buf, "Fast (%c)", vis_bar_falloff == 2 ? rb : ' '); print_str(11, 52, buf, (current_hover == 413) ? hl : bg);
            
            // Row 11: Bar Colors
            print_str(12, 17, "[-]", (current_hover == 414) ? hl : bg); 
            set_char(12, 20, '[', bg); set_char(12, 21, 254, setting_color ? vis_bar_c1 : get_accent_color()); set_char(12, 22, ']', bg); 
            print_str(12, 23, "[+]", (current_hover == 415) ? hl : bg);

            print_str(12, 40, "[-]", (current_hover == 416) ? hl : bg); 
            set_char(12, 43, '[', bg); set_char(12, 44, 254, setting_color ? vis_bar_c2 : get_accent_color()); set_char(12, 45, ']', bg); 
            print_str(12, 46, "[+]", (current_hover == 417) ? hl : bg);

            print_str(12, 63, "[-]", (current_hover == 418) ? hl : bg); 
            set_char(12, 66, '[', bg); set_char(12, 67, 254, setting_color ? vis_bar_c3 : get_accent_color()); set_char(12, 68, ']', bg); 
            print_str(12, 69, "[+]", (current_hover == 419) ? hl : bg);

            // Row 12: Bar Style
            sprintf(buf, "Solid (%c)", vis_bar_style == 0 ? rb : ' '); print_str(13, 17, buf, (current_hover == 420) ? hl : bg);
            sprintf(buf, "Shaded (%c)", vis_bar_style == 1 ? rb : ' '); print_str(13, 27, buf, (current_hover == 421) ? hl : bg);

            // Row 15: Debug
            sprintf(buf, "Live (%c)", vis_dbg_refresh == 0 ? rb : ' '); print_str(16, 17, buf, (current_hover == 422) ? hl : bg);
            sprintf(buf, "500ms (%c)", vis_dbg_refresh == 1 ? rb : ' '); print_str(16, 26, buf, (current_hover == 423) ? hl : bg);
            sprintf(buf, "1000ms (%c)", vis_dbg_refresh == 2 ? rb : ' '); print_str(16, 36, buf, (current_hover == 424) ? hl : bg);
            sprintf(buf, "Std (%c)", vis_dbg_detail == 0 ? rb : ' '); print_str(16, 61, buf, (current_hover == 425) ? hl : bg);
            sprintf(buf, "Adv (%c)", vis_dbg_detail == 1 ? rb : ' '); print_str(16, 69, buf, (current_hover == 426) ? hl : bg);

            // Row 17: FPS
            sprintf(buf, "Uncapped (%c)", vis_fps_cap == 0 ? rb : ' '); print_str(18, 18, buf, (current_hover == 427) ? hl : bg);
            sprintf(buf, "30 FPS (%c)", vis_fps_cap == 1 ? rb : ' '); print_str(18, 31, buf, (current_hover == 428) ? hl : bg);
            sprintf(buf, "15 FPS (%c)", vis_fps_cap == 2 ? rb : ' '); print_str(18, 42, buf, (current_hover == 429) ? hl : bg);

            unsigned char btn_col = (current_hover == 430) ? cr : bg;
            print_mapped_str(21, 61, "[***************]", bg);
            print_mapped_str(22, 61, "|", bg);
            print_mapped_str(22, 62, "  Save Config  ", btn_col);
            print_mapped_str(22, 77, "|", bg);
            print_mapped_str(23, 61, "{***************}", bg);
        }
    }

    last_tab = current_settings_tab;
    last_hover = current_hover;
    last_ansi = setting_ansi_mode;
    last_he = setting_hover_effects;
    last_color = setting_color;
    last_tab_elem = active_tab_element;

    if (has_mouse) show_mouse();
}

void draw_menu() {
    static int last_menu = -1;
    static int last_hover = -1;
    static int last_view = -1;
    int current_hover = -1;

    if (active_menu == 1) { 
        if (mouse_x >= 1 && mouse_x <= 16) {
            if (mouse_y == 3) current_hover = 0;
            else if (mouse_y == 4) current_hover = 1;
            else if (mouse_y == 5) current_hover = 2; // Media Info
            else if (mouse_y == 6) current_hover = 3; // Shifted Settings
            else if (mouse_y == 8) current_hover = 4; // Shifted Exit
            else if (mouse_y == 0) current_hover = 17;
            else if (mouse_y == 1) current_hover = 17; 
            else if (mouse_y == 2) current_hover = 17;  
            //else if (mouse_y == 8) current_hover = 17;
            else if (mouse_y == 9) current_hover = 17;
        }
        else if(mouse_x == 0 || mouse_x == 17){
            current_hover = 17;
        }
    } 
    else if (active_menu == 2) { 
        if (mouse_x >= 8 && mouse_x <= 32) {
            if (mouse_y == 3) current_hover = 0;
            else if (mouse_y == 4) current_hover = 1;
            else if (mouse_y == 6) current_hover = 2;
            else if (mouse_y == 7) current_hover = 3; 
            else if (mouse_y == 9) current_hover = 4; 
            else if (mouse_y == 10) current_hover = 5;
            else if (mouse_y == 0) current_hover = 17;
            else if (mouse_y == 1) current_hover = 17; 
            else if (mouse_y == 2) current_hover = 17; 
            else if (mouse_y == 11) current_hover = 17;
            else if (mouse_y == 12) current_hover = 17;
        }
        else if(mouse_x == 7 || mouse_x == 33){
            current_hover = 17;
        }
    } 
    else if (active_menu == 3) { 
        if (mouse_x >= 16 && mouse_x <= 35) {
            if (mouse_y == 3) current_hover = 0; 
            else if (mouse_y == 4) current_hover = 1; 
            else if (mouse_y == 5) current_hover = 2; 
            else if (mouse_y == 7) current_hover = 3;
            else if (mouse_y == 0) current_hover = 17;
            else if (mouse_y == 1) current_hover = 17; 
            else if (mouse_y == 2) current_hover = 17; 
            else if (mouse_y == 8) current_hover = 17;
            else if (mouse_y == 9) current_hover = 17;
        }
        else if(mouse_x == 15 || mouse_x == 36){
            current_hover = 17;
        }
    }

    if (active_menu == last_menu && current_hover == last_hover && ui_view == last_view) return;

    if (has_mouse) hide_mouse();
    
    int menu_just_opened = (active_menu != last_menu || ui_view != last_view);
    
    last_menu = active_menu;
    last_hover = current_hover;
    last_view = ui_view;
    active_menu_hover = current_hover;

    if (active_menu == 0) {
        if (has_mouse) show_mouse();
        return;
    }

    unsigned char mnu_color = get_bg_color(); 
    unsigned char hl_color = setting_hover_effects ? get_hl_color() : mnu_color;  
    unsigned char cr_color = setting_hover_effects ? get_cr_color() : mnu_color;

    if (active_menu == 1) { 
        if (menu_just_opened) {
            set_char(2, 0, 0xC3, mnu_color); set_char(2, 17, 0xC2, mnu_color); print_text(1, 1, " File ", hl_color); 
            for (int i = 0; i < 8; i++) print_text(2 + i, 0, file_menu[i], mnu_color);
        }
        
        if (active_menu_hover == 0){
            print_text(3, 1, " Open File     ", (active_menu_hover == 0) ? hl_color : mnu_color);
            print_text(4, 1, " Open Playlist ",mnu_color);
            print_text(5, 1, " Media Info    ",mnu_color);
            print_text(6, 1, " Settings      ",mnu_color);
            print_text(8, 1, " Exit          ",mnu_color);
        }
        else if (active_menu_hover == 1){
            print_text(3, 1, " Open File     ",mnu_color);
            print_text(4, 1, " Open Playlist ", (active_menu_hover == 1) ? hl_color : mnu_color);
            print_text(5, 1, " Media Info    ",mnu_color);
            print_text(6, 1, " Settings      ",mnu_color);
            print_text(8, 1, " Exit          ",mnu_color);
        }
        else if (active_menu_hover == 2){
            print_text(3, 1, " Open File     ",mnu_color);
            print_text(4, 1, " Open Playlist ",mnu_color);
            print_text(5, 1, " Media Info    ", (active_menu_hover == 2) ? hl_color : mnu_color);
            print_text(6, 1, " Settings      ",mnu_color);
            print_text(8, 1, " Exit          ",mnu_color);
        }
        else if (active_menu_hover == 3){
            print_text(3, 1, " Open File     ",mnu_color);
            print_text(4, 1, " Open Playlist ",mnu_color);
            print_text(5, 1, " Media Info    ",mnu_color);
            print_text(6, 1, " Settings      ", (active_menu_hover == 3) ? hl_color : mnu_color);
            print_text(8, 1, " Exit          ",mnu_color);
        }
        else if (active_menu_hover == 4){
            print_text(3, 1, " Open File     ",mnu_color);
            print_text(4, 1, " Open Playlist ",mnu_color);
            print_text(5, 1, " Media Info    ",mnu_color);
            print_text(6, 1, " Settings      ",mnu_color);
            print_text(8, 1, " Exit          ", (active_menu_hover == 4) ? cr_color : mnu_color);
        }
        else if (active_menu_hover == 17){
            print_text(3, 1, " Open File     ",mnu_color);
            print_text(4, 1, " Open Playlist ",mnu_color);
            print_text(5, 1, " Media Info    ",mnu_color);
            print_text(6, 1, " Settings      ",mnu_color);
            print_text(8, 1, " Exit          ",mnu_color);
        }
    }
    else if (active_menu == 2) { 
        if (menu_just_opened) {
            set_char(2, 7, 0xC2, mnu_color); set_char(2, 33, 0xC2, mnu_color); print_text(1, 8, " Audio ", hl_color); 
            for (int i = 0; i < 10; i++) print_text(2 + i, 7, audio_menu[i], mnu_color);
        }
        
        if (active_menu_hover == 0){
                print_text(3, 8, " Audio Device - SB       ", (active_menu_hover == 0) ? hl_color : mnu_color);
                print_text(4, 8, " Audio Device - PC Spkr  ",mnu_color);
                print_text(6, 8, " Volume Up               ",mnu_color);
                print_text(7, 8, " Volume Down             ",mnu_color);
                print_text(9, 8, " Toggle Loop             ",mnu_color);
                print_text(10, 8, " Audio Settings          ",mnu_color);
        }
        else if (active_menu_hover == 1){
                print_text(3, 8, " Audio Device - SB       ",mnu_color);
                print_text(4, 8, " Audio Device - PC Spkr  ", (active_menu_hover == 1) ? hl_color : mnu_color);
                print_text(6, 8, " Volume Up               ",mnu_color);
                print_text(7, 8, " Volume Down             ",mnu_color);
                print_text(9, 8, " Toggle Loop             ",mnu_color);
                print_text(10, 8, " Audio Settings          ",mnu_color);
        }
        else if (active_menu_hover == 2){
                print_text(3, 8, " Audio Device - SB       ",mnu_color);
                print_text(4, 8, " Audio Device - PC Spkr  ",mnu_color);
                print_text(6, 8, " Volume Up               ", (active_menu_hover == 2) ? hl_color : mnu_color);
                print_text(7, 8, " Volume Down             ",mnu_color);
                print_text(9, 8, " Toggle Loop             ",mnu_color);
                print_text(10, 8, " Audio Settings          ",mnu_color);
        }
        else if (active_menu_hover == 3){
                print_text(3, 8, " Audio Device - SB       ",mnu_color);
                print_text(4, 8, " Audio Device - PC Spkr  ",mnu_color);
                print_text(6, 8, " Volume Up               ",mnu_color);
                print_text(7, 8, " Volume Down             ", (active_menu_hover == 3) ? hl_color : mnu_color);
                print_text(9, 8, " Toggle Loop             ",mnu_color);
                print_text(10, 8, " Audio Settings          ",mnu_color);
        }
        else if (active_menu_hover == 4){
                print_text(3, 8, " Audio Device - SB       ",mnu_color);
                print_text(4, 8, " Audio Device - PC Spkr  ",mnu_color);
                print_text(6, 8, " Volume Up               ",mnu_color);
                print_text(7, 8, " Volume Down             ",mnu_color);
                print_text(9, 8, " Toggle Loop             ", (active_menu_hover == 4) ? hl_color : mnu_color);
                print_text(10, 8, " Audio Settings          ",mnu_color);
        }
        else if (active_menu_hover == 5){
                print_text(3, 8, " Audio Device - SB       ",mnu_color);
                print_text(4, 8, " Audio Device - PC Spkr  ",mnu_color);
                print_text(6, 8, " Volume Up               ",mnu_color);
                print_text(7, 8, " Volume Down             ",mnu_color);
                print_text(9, 8, " Toggle Loop             ",mnu_color);
                print_text(10, 8, " Audio Settings          ", (active_menu_hover == 5) ? hl_color : mnu_color);
        }

        else if (active_menu_hover == 17){
            print_text(3, 8, " Audio Device - SB       ",mnu_color);
            print_text(4, 8, " Audio Device - PC Spkr  ",mnu_color);
            print_text(6, 8, " Volume Up               ",mnu_color);
            print_text(7, 8, " Volume Down             ",mnu_color);
            print_text(9, 8, " Toggle Loop             ",mnu_color);
            print_text(10, 8, " Audio Settings          ",mnu_color);
        }
    }
    else if (active_menu == 3) { 
        if (menu_just_opened) {
            set_char(2, 15, 0xC2, mnu_color); set_char(2, 36, 0xC2, mnu_color); print_text(1, 16, " Visual ", hl_color); 
            for (int i = 0; i < 7; i++) print_text(2 + i, 15, visual_menu[i], mnu_color);
        }
        if (active_menu_hover == 0){
            print_text(3, 16, " VU Meter           ", (active_menu_hover == 0) ? hl_color : mnu_color);
            print_text(4, 16, " Frequency Bars     ",mnu_color);
            print_text(5, 16, " Debug Info         ",mnu_color);
            print_text(7, 16, " Visual Settings    ",mnu_color);
        }
        else if (active_menu_hover == 1){
            print_text(3, 16, " VU Meter           ",mnu_color);
            print_text(4, 16, " Frequency Bars     ", (active_menu_hover == 1) ? hl_color : mnu_color);
            print_text(5, 16, " Debug Info         ",mnu_color);
            print_text(7, 16, " Visual Settings    ",mnu_color);
        }
        else if (active_menu_hover == 2){
            print_text(3, 16, " VU Meter           ",mnu_color);
            print_text(4, 16, " Frequency Bars     ",mnu_color);
            print_text(5, 16, " Debug Info         ", (active_menu_hover == 2) ? hl_color : mnu_color);
            print_text(7, 16, " Visual Settings    ",mnu_color);
        }
        else if (active_menu_hover == 3){
            print_text(3, 16, " VU Meter           ",mnu_color);
            print_text(4, 16, " Frequency Bars     ",mnu_color);
            print_text(5, 16, " Debug Info         ",mnu_color);
            print_text(7, 16, " Visual Settings    ", (active_menu_hover == 3) ? hl_color : mnu_color);
        }
        else if (active_menu_hover == 17){
            print_text(3, 16, " VU Meter           ",mnu_color);
            print_text(4, 16, " Frequency Bars     ",mnu_color);
            print_text(5, 16, " Debug Info         ",mnu_color);
            print_text(7, 16, " Visual Settings    ",mnu_color);
        }

    }
    
    if (has_mouse) show_mouse();
}

void load_new_file_from_browser(const char* filepath) {
    if (has_mouse) hide_mouse();
    is_paused = 1; if (ui_view != 3) ui_view = 0; force_ui_redraw = 1; visualizer_mode = 3; clear_inner_ui();

    int found_idx = -1;
    for(int i=0; i<queue_count; i++) { if (strcmp(queue_paths[i], filepath) == 0) { found_idx = i; break; } }
    if (found_idx == -1 && queue_count < MAX_QUEUE) {
        strncpy(queue_paths[queue_count], filepath, 511); strncpy(queue_displays[queue_count], get_basename(filepath), 63); queue_current = queue_count; queue_count++;
    } else if (found_idx != -1) queue_current = found_idx;
    
    strncpy(app_filename, filepath, 511); app_filename[511] = '\0';
    init_ui(app_filename); draw_visualizer(); if (has_mouse) show_mouse();

    if (active_audio_file) { fclose(active_audio_file); active_audio_file = NULL; }
    
    // --- NEW: DUAL HEADER PARSING ---
    start_off = 0; file_is_native_wav = 0; current_bitdepth = 16; wav_data_size = 0;
    current_sample_rate = parse_wav_header(filepath, &start_off, &current_channels, &current_bitdepth, &wav_data_size);
    if (current_sample_rate > 0) {
        file_is_native_wav = 1; file_is_vbr = 0;
        file_bitrate_kbps = (current_sample_rate * current_channels * current_bitdepth) / 1000;
        file_size = start_off + wav_data_size;
        ui_total_seconds = wav_data_size / (current_channels * (current_bitdepth / 8) * current_sample_rate);
    } else {
        start_off = 0;
        current_sample_rate = parse_mp3_header(filepath, &start_off, &current_channels);
        if (!current_sample_rate) current_sample_rate = 44100;
    }
    // --------------------------------

    target_sample_rate = global_out_sample_rate; target_channels = global_out_channels;
    resample_step = ((unsigned long long)current_sample_rate << 16) / target_sample_rate; resample_pos = 0;
    
    FILE *mp3_file = fopen(filepath, "rb");
    if (!mp3_file) { strcpy(browser_status_msg, "Error: Could not open file!"); has_active_file = 0; return; }
    fseek(mp3_file, 0, SEEK_END); file_size = ftell(mp3_file); 
    if (start_off > file_size) start_off = 0; 
    
    if (global_is_486 && !file_is_native_wav) {
        int conversion_success = 0, active_divisor = CONF_486_DIVISOR;
        while (!conversion_success && active_divisor <= 4) {
            if (!global_is_pc_speaker && custom_sample_rate == 0) { 
                target_sample_rate = current_sample_rate / active_divisor; 
                if (is_sb16 && target_sample_rate > 44100) target_sample_rate = 44100; 
                
                // --- APPLY SB PRO 1MHz HARDWARE LIMITS TO TRANSCODER ---
                if (!is_sb16 && dsp_major == 3) {
                    if (target_channels == 2 && target_sample_rate > 22050) target_sample_rate = 22050;
                    if (target_channels == 1 && target_sample_rate > 44100) target_sample_rate = 44100;
                    int tc = 256 - (1000000 / (target_channels == 2 ? target_sample_rate * 2 : target_sample_rate));
                    target_sample_rate = (target_channels == 2) ? (1000000 / (256 - tc)) / 2 : (1000000 / (256 - tc));
                }
                // -------------------------------------------------------
                
                resample_step = ((unsigned long long)current_sample_rate << 16) / target_sample_rate; 
            }
            if (has_mouse) hide_mouse(); print_fmt(17, 6, get_accent_color(), "[ 486 OPTIMIZATION: Transcoding MP3 to WAV (1/%d Quality) ]", active_divisor); if (has_mouse) show_mouse();
            
            FILE *wav_file = fopen("486_temp.wav", "wb+");
            if (wav_file) {
                unsigned char dummy_header[44] = {0}; fwrite(dummy_header, 1, 44, wav_file);
                mp3dec_t temp_mp3d; mp3dec_init(&temp_mp3d); mp3dec_frame_info_t temp_info; 
                
                // Mount MP3 to our streaming engine temporarily!
                active_audio_file = mp3_file; fseek(active_audio_file, start_off, SEEK_SET);
                stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); current_ram_ptr = stream_buffer;
                long trans_bytes_left = file_size - start_off; int total_wav_bytes = 0, disk_full = 0;
                
                while (trans_bytes_left > 0) {
                    refill_stream(); // Uses the 64K sliding window!
                    int avail = stream_bytes - (current_ram_ptr - stream_buffer);
                    int samples = mp3dec_decode_frame(&temp_mp3d, current_ram_ptr, avail, pcm_temp, &temp_info);
                    if (samples > 0) {
                        int out_samples = samples; short* final_pcm_ptr = pcm_temp;
                        if (current_sample_rate != target_sample_rate || current_channels != target_channels) {
                            out_samples = 0; unsigned int temp_resample_pos = 0;
                            while ((temp_resample_pos >> 16) < samples) {
                                int src_idx = temp_resample_pos >> 16; unsigned int frac = temp_resample_pos & 0xFFFF;
                                if (src_idx >= samples - 1) { if (target_channels == 2 && current_channels == 2) { resampled_temp[out_samples*2] = pcm_temp[src_idx*2]; resampled_temp[out_samples*2+1] = pcm_temp[src_idx*2+1]; } else if (target_channels == 1 && current_channels == 2) { resampled_temp[out_samples] = (pcm_temp[src_idx*2] >> 1) + (pcm_temp[src_idx*2+1] >> 1); } else if (target_channels == 2 && current_channels == 1) { resampled_temp[out_samples*2] = pcm_temp[src_idx]; resampled_temp[out_samples*2+1] = pcm_temp[src_idx]; } else resampled_temp[out_samples] = pcm_temp[src_idx]; } 
                                else { if (target_channels == 2 && current_channels == 2) { int L0=pcm_temp[src_idx*2], L1=pcm_temp[src_idx*2+2], R0=pcm_temp[src_idx*2+1], R1=pcm_temp[src_idx*2+3]; resampled_temp[out_samples*2] = L0 + (((L1 - L0) * (int)frac) >> 16); resampled_temp[out_samples*2+1] = R0 + (((R1 - R0) * (int)frac) >> 16); } else if (target_channels == 1 && current_channels == 2) { int L0=pcm_temp[src_idx*2], L1=pcm_temp[src_idx*2+2], R0=pcm_temp[src_idx*2+1], R1=pcm_temp[src_idx*2+3]; int M0=(L0>>1)+(R0>>1), M1=(L1>>1)+(R1>>1); resampled_temp[out_samples] = M0 + (((M1 - M0) * (int)frac) >> 16); } else if (target_channels == 2 && current_channels == 1) { int S0=pcm_temp[src_idx], S1=pcm_temp[src_idx+1]; int val = S0 + (((S1 - S0) * (int)frac) >> 16); resampled_temp[out_samples*2] = val; resampled_temp[out_samples*2+1] = val; } else { int S0=pcm_temp[src_idx], S1=pcm_temp[src_idx+1]; resampled_temp[out_samples] = S0 + (((S1 - S0) * (int)frac) >> 16); } }
                                out_samples++; temp_resample_pos += resample_step;
                            }
                            final_pcm_ptr = resampled_temp;
                        }
                        int final_total_samples = out_samples * target_channels; int expected_bytes = 0, written_bytes = 0;
                        if (active_bitdepth == 8) { unsigned char out_8bit[MINIMP3_MAX_SAMPLES_PER_FRAME * 2]; for (int i = 0; i < final_total_samples; i++) out_8bit[i] = (unsigned char)((final_pcm_ptr[i] >> 8) + 128); expected_bytes = final_total_samples; written_bytes = fwrite(out_8bit, 1, expected_bytes, wav_file); } else { expected_bytes = final_total_samples * 2; written_bytes = fwrite(final_pcm_ptr, 1, expected_bytes, wav_file); }
                        if (written_bytes != expected_bytes) { disk_full = 1; break; } total_wav_bytes += written_bytes;
                    }
                    if (disk_full) break;
                    if (temp_info.frame_bytes > 0) { current_ram_ptr += temp_info.frame_bytes; trans_bytes_left -= temp_info.frame_bytes; } else { current_ram_ptr++; trans_bytes_left--; }
                }
                if (disk_full) { fclose(wav_file); remove("486_temp.wav"); active_divisor++; continue; }
                rewind(wav_file); write_wav_header(wav_file, target_sample_rate, target_channels, active_bitdepth, total_wav_bytes);
                
                global_wav_bytes = total_wav_bytes; ui_total_seconds = total_wav_bytes / (target_sample_rate * target_channels * (active_bitdepth / 8)); conversion_success = 1;
                
                // Mount the WAV file for streaming playback!
                fclose(mp3_file);
                active_audio_file = wav_file; 
                fseek(active_audio_file, 44, SEEK_SET); // Skip WAV header
            } else return;
            if (global_is_pc_speaker || custom_sample_rate > 0) break;
        }
        if (!conversion_success) { if(active_audio_file) fclose(active_audio_file); return; }
    } else {
        // Mount the MP3 file for streaming playback!
        active_audio_file = mp3_file;
        fseek(active_audio_file, start_off, SEEK_SET);
        stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file);
        current_ram_ptr = stream_buffer;
        
        if (!file_is_native_wav) {
            mp3dec_init(&mp3d); mp3dec_frame_info_t temp_info; mp3dec_decode_frame(&mp3d, current_ram_ptr, stream_bytes, pcm_temp, &temp_info); 
            int vbr_frames = 0; file_is_vbr = 0;
            for (int i = 0; i < 200 && i < stream_bytes - 12; i++) {
                if ((current_ram_ptr[i] == 'X' && current_ram_ptr[i+1] == 'i' && current_ram_ptr[i+2] == 'n' && current_ram_ptr[i+3] == 'g') || (current_ram_ptr[i] == 'I' && current_ram_ptr[i+1] == 'n' && current_ram_ptr[i+2] == 'f' && current_ram_ptr[i+3] == 'o')) {
                    int flags = (current_ram_ptr[i+4] << 24) | (current_ram_ptr[i+5] << 16) | (current_ram_ptr[i+6] << 8) | current_ram_ptr[i+7];
                    if (flags & 1) { vbr_frames = (current_ram_ptr[i+8] << 24) | (current_ram_ptr[i+9] << 16) | (current_ram_ptr[i+10] << 8) | current_ram_ptr[i+11]; file_is_vbr = 1; } break;
                }
            }
            if (vbr_frames > 0 && current_sample_rate > 0) { int samples_per_frame = (current_sample_rate >= 32000) ? 1152 : 576; long total_samples_vbr = (long)vbr_frames * samples_per_frame; ui_total_seconds = total_samples_vbr / current_sample_rate; file_bitrate_kbps = (file_size * 8) / (ui_total_seconds > 0 ? ui_total_seconds * 1000 : 1); } else if (temp_info.bitrate_kbps > 0) { ui_total_seconds = (file_size * 8) / (temp_info.bitrate_kbps * 1000); file_bitrate_kbps = temp_info.bitrate_kbps; }
            mp3dec_init(&mp3d);
        }
    }
    
    has_id3 = 0; memset(id3_title, 0, 31); memset(id3_artist, 0, 31); parse_id3(filepath);
    
    if (has_mouse) hide_mouse();
    print_text(16, 6, "                                                  ", get_bg_color()); print_text(17, 6, "                                                                ", get_bg_color()); force_ui_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse();
    ui_total_samples_played = 0; frames_decoded = 0; buffer_skips = 0; pcm_leftover_bytes = 0; has_active_file = 1; is_paused = 0; strcpy(browser_status_msg, ""); 
}

void save_playlist_m3u(const char* filepath) { // <--- ADD PARAMETER
    FILE* f = fopen(filepath, "w"); // <--- USE PARAMETER
    if (f) {
        fprintf(f, "#EXTM3U\n");
        for (int i = 0; i < queue_count; i++) {
            fprintf(f, "%s\n", queue_paths[i]);
        }
        fclose(f);
        sprintf(browser_status_msg, "Saved as %s", get_basename(filepath)); // <--- SHOW NEW NAME
    } else {
        strcpy(browser_status_msg, "Error saving playlist!");
    }
}

void load_playlist_m3u(const char* filepath, int append) {
    FILE* f = fopen(filepath, "r");
    if (!f) {
        strcpy(browser_status_msg, "Error opening playlist!");
        return;
    }
    
    if (!append) { // If normal open, wipe the existing queue
        queue_count = 0; queue_current = -1; queue_selected = -1; queue_scroll = 0;
    }
    
    char line[1024];
    int missing_count = 0, added_count = 0;
    
    while (fgets(line, sizeof(line), f)) {
        // Trim standard invisible newline/carriage return characters
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[len-1] = '\0';
            len--;
        }
        
        if (len == 0 || line[0] == '#') continue; // Skip empty lines and #EXTINF metadata
        
        if (access(line, 0) == 0) { // Ping the disk. Does the file exist?
            if (queue_count < MAX_QUEUE) {
                strncpy(queue_paths[queue_count], line, 511);
                strncpy(queue_displays[queue_count], get_basename(line), 63);
                queue_count++;
                added_count++;
            }
        } else {
            missing_count++; // File doesn't exist, skip it!
        }
    }
    fclose(f);
    
    if (missing_count > 0) sprintf(browser_status_msg, "Loaded %d. %d missing/skipped.", added_count, missing_count);
    else sprintf(browser_status_msg, "Loaded %d tracks from playlist.", added_count);
}

void execute_browser_ok() {
    if (strlen(dialog_file_name) > 0) {
        int target_is_dir = 0;
        int target_exists = 0;
        
        if (strcmp(dialog_file_name, "..") == 0) {
            target_is_dir = 1;
            target_exists = 1;
        } else {
            for (int i = 0; i < all_file_count; i++) {
                int j = 0;
                int match = 1;
                while (all_file_list[i].name[j] != '\0' || dialog_file_name[j] != '\0') {
                    unsigned char c1 = (unsigned char)all_file_list[i].name[j];
                    unsigned char c2 = (unsigned char)dialog_file_name[j];
                    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                    if (c1 != c2) {
                        match = 0;
                        break;
                    }
                    j++;
                }
                if (match) {
                    target_exists = 1;
                    target_is_dir = all_file_list[i].is_dir;
                    strcpy(dialog_file_name, all_file_list[i].name); 
                    break;
                }
            }
        }

        if (!target_exists && !browser_save_mode) { // <--- ALLOW BYPASS IF SAVING
            strcpy(browser_status_msg, "File doesnt exist");
            browser_needs_redraw = 1;
            return; 
        }

        if (target_is_dir) {
            if (has_mouse) hide_mouse();
            if (strcmp(dialog_file_name, "..") == 0) {
                char *last_slash = strrchr(current_dir, '/');
                char *last_bslash = strrchr(current_dir, '\\');
                char *slash = (last_slash > last_bslash) ? last_slash : last_bslash;
                if (slash) {
                    if (slash == current_dir || *(slash - 1) == ':') *(slash + 1) = '\0';
                    else *slash = '\0';
                }
            } else {
                int len = strlen(current_dir);
                if (len + strlen(dialog_file_name) + 2 < 512) {
                    if (len > 0 && current_dir[len-1] != '/' && current_dir[len-1] != '\\') {
                        strcat(current_dir, "\\");
                    }
                    strcat(current_dir, dialog_file_name);
                }
            }
            strcpy(browser_status_msg, ""); 
            load_dir(current_dir, 1);
            if (has_mouse) show_mouse();
        } else {
            char target_filepath[1024];
            int len = strlen(current_dir);
            if (len > 0 && current_dir[len-1] != '/' && current_dir[len-1] != '\\') {
                snprintf(target_filepath, 1024, "%s\\%s", current_dir, dialog_file_name);
            } else {
                snprintf(target_filepath, 1024, "%s%s", current_dir, dialog_file_name);
            }
            
            // --- NEW SAVE INTERCEPT LOGIC ---
            if (browser_save_mode) {
                // Auto-append .m3u if the user forgot to type it
                if (!ends_with_ignore_case(target_filepath, ".m3u") && !ends_with_ignore_case(target_filepath, ".m3u8")) {
                    strcat(target_filepath, ".m3u");
                }
                save_playlist_m3u(target_filepath);
                
                // Route user directly back to the Playlist view
                browser_save_mode = 0;
                if (has_mouse) hide_mouse();
                ui_view = 3; 
                force_ui_redraw = 1;
                init_ui(app_filename); 
                if (has_mouse) show_mouse();
                return;
            }
            // --------------------------------
            
            // --- NEW M3U INTERCEPT LOGIC ---
            int is_m3u = ends_with_ignore_case(target_filepath, ".m3u") || ends_with_ignore_case(target_filepath, ".m3u8");
            
            if (is_m3u) {
                int is_append = (browser_enqueue_only && has_active_file);
                load_playlist_m3u(target_filepath, is_append);
                browser_needs_redraw = 1;
                
                // If normal open (not enqueue) and tracks loaded, instantly start track 1!
                if (!is_append && queue_count > 0) {
                    load_new_file_from_browser(queue_paths[0]);
                }
            } else {
                // --- EXISTING ENQUEUE LOGIC ---
                if (browser_enqueue_only && has_active_file) {
                    int found_idx = -1;
                    for(int i=0; i<queue_count; i++) {
                        if (strcmp(queue_paths[i], target_filepath) == 0) { found_idx = i; break; }
                    }
                    if (found_idx == -1 && queue_count < MAX_QUEUE) {
                        strncpy(queue_paths[queue_count], target_filepath, 511);
                        strncpy(queue_displays[queue_count], get_basename(target_filepath), 63);
                        queue_count++;
                        strcpy(browser_status_msg, "Added to playlist queue!");
                    } else if (found_idx != -1) {
                        strcpy(browser_status_msg, "Already in playlist!");
                    } else {
                        strcpy(browser_status_msg, "Queue is full!");
                    }
                    browser_needs_redraw = 1; 
                } 
                else {
                    load_new_file_from_browser(target_filepath); // Normal instant-play
                }
            }
        }
    }
}

void update_ui(int current_sec, int total_sec) {
    if (ui_view == 0 || ui_view == 3 || ui_view == 4) {
        unsigned char bg = get_bg_color();
        unsigned char dim = get_dim_color();
        unsigned char ac = get_accent_color();
        // Unified Hover Engine for Main UI elements
        
        static int last_sec = -1, last_tot = -1, last_vol = -1, last_pause = -1, last_loop = -1;
        static int last_hover = -1;

        if (is_paused) {
            force_ui_redraw = 1;
        }
        
        if (force_ui_redraw) {
            last_sec = -1; last_tot = -1; last_vol = -1; last_pause = -1; last_loop = -1; last_hover = -1;
            force_ui_redraw = 0;
        }
        
        // Progress Bar
        if (current_sec != last_sec || total_sec != last_tot) {
            int mouse_in_prog = (has_mouse && mouse_y == 19);
            if (mouse_in_prog) hide_mouse();
            print_fmt(19, 2, bg, "%02d:%02d", current_sec / 60, current_sec % 60);
            print_fmt(19, 73, bg, "%02d:%02d", total_sec / 60, total_sec % 60);
            
            int fill_width = (total_sec > 0) ? (current_sec * 62) / total_sec : 0;
            if (fill_width > 62) fill_width = 62; if (fill_width < 0) fill_width = 0;
            for (int i = 0; i < 62; i++) {
                if (i < fill_width) {
                    // Solid filled portion (Cyan in Color Mode, Standard Accent in Monochrome)
                    unsigned char fill_col = setting_color ? 0x3B : ac;
                    set_char(19, 9 + i, 0xDB, fill_col);
                }
                else if (i == fill_width) {
                    // The glowing leading edge!
                    unsigned char glow_col = setting_color ? ((bg & 0xF0) | 3) : dim; 
                    set_char(19, 9 + i, 177, glow_col); 
                }
                else {
                    // Empty track
                    set_char(19, 9 + i, 176, dim); 
                }
            }
            if (mouse_in_prog) show_mouse();
            last_sec = current_sec; last_tot = total_sec;
        }

        int current_hover = 0; //Clean Slate Default!

        if (setting_hover_effects && active_menu == 0) { 
            if (mouse_y == 1) {
                if (mouse_x >= 1 && mouse_x <= 6) current_hover = 2; // File
                else if (mouse_x >= 8 && mouse_x <= 14) current_hover = 3; // Audio
                else if (mouse_x >= 16 && mouse_x <= 23) current_hover = 4; // Visual
                else if (mouse_x >= 76 && mouse_x <= 78) current_hover = 1; // X
            } 
            else if (mouse_y == 22) { 
                if (mouse_x >= 3 && mouse_x <= 5) current_hover = 5; // Playlist
                else if (mouse_x >= 9 && mouse_x <= 11) current_hover = 6; // Settings
                else if (mouse_x >= 21 && mouse_x <= 23) current_hover = 7; // Stop
                else if (mouse_x >= 27 && mouse_x <= 31) current_hover = 8; // Rewind
                else if (mouse_x >= 35 && mouse_x <= 43) current_hover = 9; // Play/Pause
                else if (mouse_x >= 46 && mouse_x <= 52) current_hover = 10; // Fwd
                else if (mouse_x >= 54 && mouse_x <= 58) current_hover = 11; // Loop
                else if (mouse_x >= 68 && mouse_x <= 70) current_hover = 12; // Vol -
                else if (mouse_x >= 74 && mouse_x <= 76) current_hover = 13; // Vol +
            }
        }

        if (active_menu == 0) {
            draw_visualizer();
        }


        // --- MASTER REDRAW WRAPPER ---
        if (current_hover != last_hover || is_paused != last_pause || is_looping != last_loop || master_volume != last_vol) {
            if (has_mouse) hide_mouse();
            
            // If last_hover is -1, force_ui_redraw wiped the state, so we draw everything once.
            int redraw_all = (last_hover == -1);

            // --- PILLAR 5: GROUPED REDRAW FOR TOP MENU (IDs 1 to 4) ---
            if (redraw_all || (current_hover >= 1 && current_hover <= 4) || (last_hover >= 1 && last_hover <= 4)) {
                print_text(1, 75, "\xB3", bg); // UI Separator
                print_text(1, 76, " X ", (current_hover == 1) ? get_cr_color() : bg);
                print_text(1, 1, " File ", (current_hover == 2 || active_menu == 1) ? get_hl_color() : bg);
                print_text(1, 8, " Audio ", (current_hover == 3 || active_menu == 2) ? get_hl_color() : bg);
                print_text(1, 16, " Visual ", (current_hover == 4 || active_menu == 3) ? get_hl_color() : bg);
            }

            // --- PILLAR 5: GROUPED REDRAW FOR BOTTOM CONTROLS (IDs 5 to 13) ---
            if (redraw_all || (current_hover >= 5 && current_hover <= 13) || (last_hover >= 5 && last_hover <= 13) || 
                is_paused != last_pause || is_looping != last_loop || master_volume != last_vol) {
                
                draw_player_btn(2, 5, " \xF0 ", bg, (current_hover == 5) ? get_hl_color() : bg);
                draw_player_btn(8, 5, " \x0F ", bg, (current_hover == 6) ? get_hl_color() : bg);
                draw_player_btn(20, 5, " \xFE ", bg, (current_hover == 7) ? get_cr_color() : bg);
                draw_player_btn(26, 7, " \x11\x11\x7C ", bg, (current_hover == 8) ? get_hl_color() : bg);
                
                // Play/Pause Button (Evaluates pause state natively!)
                unsigned char pp_icon_col;
                if (current_hover == 9) {
                    pp_icon_col = get_hl_color();
                } else if (setting_color) {
                    pp_icon_col = is_paused ? ((bg & 0xF0) | 12) : ((bg & 0xF0) | 10);
                } else {
                    pp_icon_col = ac; 
                }
                
                if (!setting_ansi_mode) draw_player_btn(34, 11, " |> / || ", bg, pp_icon_col);
                else draw_player_btn(34, 11, "  \x10 / || ", bg, pp_icon_col);
                
                draw_player_btn(46, 7, " \x7C\x10\x10 ", bg, (current_hover == 10) ? get_hl_color() : bg);
                
                // Loop Button (Evaluates loop state natively!)
                unsigned char loop_icon_col = (current_hover == 11) ? get_hl_color() : (is_looping ? ac : dim);
                draw_player_btn(54, 5, " \xEC ", bg, loop_icon_col);
                
                // Volume Controls (Evaluates volume natively!)
                char vol_buf[16];
                if (master_volume == 100) sprintf(vol_buf, "FF%%");
                else sprintf(vol_buf, "%2d%%", master_volume);
                
                draw_player_btn2(67, 11, vol_buf, bg, 7);
                print_text(22, 68, " - ", (current_hover == 12) ? get_hl_color() : bg);
                print_text(22, 71, vol_buf, bg);
                print_text(22, 74, " + ", (current_hover == 13) ? get_hl_color() : bg);
            }

            if (has_mouse) show_mouse();
            
            // --- PILLAR 6: THE STATE LOCK ---
            last_hover = current_hover;
            last_pause = is_paused;
            last_loop = is_looping;
            last_vol = master_volume;
        }
    }
    if (ui_view == 1) {
        draw_file_browser();
    }
    else if (ui_view == 2) {
        draw_settings();
    }
    else if (ui_view == 3) {  
        draw_playlist();
    }
    else if (ui_view == 4) {
        draw_media_info(); 
    } 
    draw_menu();
}

int init_audio_engine(){
    global_out_sample_rate = target_sample_rate; global_out_channels = target_channels;
    if (has_active_file) parse_id3(app_filename); 
    set_volume(master_volume); init_ui(app_filename); init_mouse(); 

    if (global_is_pc_speaker) {//pc speaker output
        memset(pc_speaker_dma, 128, active_buffer_size);
        _go32_dpmi_lock_code((void *)speaker_handler, (unsigned long)speaker_handler_end - (unsigned long)speaker_handler);
        _go32_dpmi_lock_data((void *)&interrupt_count, sizeof(interrupt_count));
        _go32_dpmi_lock_data((void *)&refill_request, sizeof(refill_request));
        _go32_dpmi_lock_data((void *)&dma_play_pos, sizeof(dma_play_pos));
        _go32_dpmi_lock_data((void *)&pc_speaker_dma, sizeof(pc_speaker_dma));
        _go32_dpmi_lock_data((void *)&pit_divisor, sizeof(pit_divisor));
        _go32_dpmi_lock_data((void *)&master_volume, sizeof(master_volume));
        _go32_dpmi_lock_data((void *)&active_buffer_size, sizeof(active_buffer_size));
        _go32_dpmi_lock_data((void *)&pc_speaker_overdrive, sizeof(pc_speaker_overdrive));
        new_isr.pm_offset = (unsigned long)speaker_handler;
        new_isr.pm_selector = _go32_my_cs();
        _go32_dpmi_get_protected_mode_interrupt_vector(0x08, &old_isr);
        _go32_dpmi_allocate_iret_wrapper(&new_isr);
        _go32_dpmi_set_protected_mode_interrupt_vector(0x08, &new_isr);
        outportb(0x43, 0x36); outportb(0x40, pit_divisor & 0xFF);
        outportb(0x40, (pit_divisor >> 8) & 0xFF); outportb(0x43, 0x90); outportb(0x61, inportb(0x61) | 0x03); 
    } 
    else {//sound blaster output
        if (!setup_dma()) return 1;
        
        // Critical: Lock all variables the interrupt relies on to prevent DPMI page faults
        _go32_dpmi_lock_code((void *)sb_handler, (unsigned long)sb_handler_end - (unsigned long)sb_handler); 
        _go32_dpmi_lock_data((void *)&interrupt_count, sizeof(interrupt_count)); 
        _go32_dpmi_lock_data((void *)&refill_request, sizeof(refill_request));
        _go32_dpmi_lock_data((void *)&active_bitdepth, sizeof(active_bitdepth));
        _go32_dpmi_lock_data((void *)&dsp_major, sizeof(dsp_major));
        _go32_dpmi_lock_data((void *)&has_auto_init, sizeof(has_auto_init));
        _go32_dpmi_lock_data((void *)&global_is_pc_speaker, sizeof(global_is_pc_speaker));
        _go32_dpmi_lock_data((void *)&active_buffer_size, sizeof(active_buffer_size));
        _go32_dpmi_lock_data((void *)&physical_addr, sizeof(physical_addr));

        new_isr.pm_offset = (unsigned long)sb_handler; new_isr.pm_selector = _go32_my_cs(); _go32_dpmi_get_protected_mode_interrupt_vector(IRQ_VECTOR, &old_isr); _go32_dpmi_allocate_iret_wrapper(&new_isr); _go32_dpmi_set_protected_mode_interrupt_vector(IRQ_VECTOR, &new_isr);
        
        outportb(0x21, inportb(0x21) & ~(1 << SB_IRQ)); // Unmask IRQ
        
        write_dsp(0xD1); // Speaker ON
        
        if (is_sb16) {
            write_dsp(0x41); write_dsp((unsigned char)((target_sample_rate >> 8) & 0xFF)); write_dsp((unsigned char)(target_sample_rate & 0xFF));
            unsigned int block_size = ((active_buffer_size / 2) / 2) - 1; // 16-bit block format
            int dsp_cmd = (target_channels == 2) ? 0xB6 : 0xC6; 
            int dsp_mode = (target_channels == 2) ? 0x30 : 0x10; 
            write_dsp(dsp_cmd); write_dsp(dsp_mode); write_dsp((unsigned char)(block_size & 0xFF)); write_dsp((unsigned char)((block_size >> 8) & 0xFF));
        } else {
            if (dsp_major == 3) {
                // Flip SB Pro hardware stereo bits
                outportb(MIXER_ADDR, 0x0E);
                unsigned char val = inportb(MIXER_DATA);
                outportb(MIXER_ADDR, 0x0E);
                outportb(MIXER_DATA, (target_channels == 2) ? (val | 0x02) : (val & ~0x02));
            }
            
            // Calculate the physical byte output rate (Stereo sends 2 bytes per sample)
            // 1. Keep your original stereo logic exactly as it is!
            int output_rate = (target_channels == 2) ? target_sample_rate * 2 : target_sample_rate;
            
            // 2. NEW: Add rounding to the Time Constant to prevent emulator overshoot!
            int tc = 256 - ((1000000 + (output_rate / 2)) / output_rate);
            
            // 3. NEW: Reverse the math to find the ACTUAL rate the hardware will use
            int actual_output_rate = 1000000 / (256 - tc);
            
            // 4. NEW: Divide it back down if it's stereo to find the true per-channel rate, 
            // and feed that back to your software resampler!
            target_sample_rate = (target_channels == 2) ? (actual_output_rate / 2) : actual_output_rate;

            // 5. Send to DSP
            write_dsp(0x40); 
            write_dsp((unsigned char)tc);
            
            unsigned int block_size = (active_buffer_size / 2) - 1; // 8-bit block format
            
            if (!has_auto_init) {
                // DSP 1.x and DSP 2.00: Single-cycle.
                if (output_rate > 21739) {
                    write_dsp(0x48); write_dsp((unsigned char)(block_size & 0xFF)); write_dsp((unsigned char)((block_size >> 8) & 0xFF));
                    write_dsp(0x91); // High-speed single-cycle
                } else {
                    write_dsp(0x14); write_dsp((unsigned char)(block_size & 0xFF)); write_dsp((unsigned char)((block_size >> 8) & 0xFF));
                }
            } else {
                // DSP 2.01+ and 3.xx
                write_dsp(0x48); write_dsp((unsigned char)(block_size & 0xFF)); write_dsp((unsigned char)((block_size >> 8) & 0xFF));
                if (output_rate > 23000) {
                    write_dsp(0x90); // HIGH SPEED auto-init
                } else {
                    write_dsp(0x1C); // Normal speed auto-init
                }
            }
        }
    }
    return 0;
}

void close_audio_engine(){
    if (global_is_pc_speaker) {
        outportb(0x43, 0x36); 
        outportb(0x40, 0x00);
        outportb(0x40, 0x00);
        outportb(0x61, inportb(0x61) & ~0x03); 
        _go32_dpmi_set_protected_mode_interrupt_vector(0x08, &old_isr);
    } else { 
        // 1. Force the Sound Blaster into a hard sleep state
        reset_dsp(); 
        
        // 2. Tell the motherboard DMA controller to stop transferring bytes
        outportb(DMA8_MASK, 0x05);  // Mask DMA Channel 1
        outportb(DMA16_MASK, 0x05); // Mask DMA Channel 5

        // 3. Mask the IRQ in the PIC so the card stops throwing interrupts at DOS
        outportb(0x21, inportb(0x21) | (1 << SB_IRQ));

        // 4. Safely unhook our interrupt vector and restore the DOS default
        _go32_dpmi_set_protected_mode_interrupt_vector(IRQ_VECTOR, &old_isr); 
        
        // 5. Free the hardware memory buffer
        _go32_dpmi_free_dos_memory(&dos_buffer); 
    }

    _go32_dpmi_free_iret_wrapper(&new_isr);
    
    // 6. Free our RAM and nullify pointers so the next load starts clean
    if (mp3_ram_cache) { free(mp3_ram_cache); mp3_ram_cache = NULL; }
    if (pcm_ram_cache) { free(pcm_ram_cache); pcm_ram_cache = NULL; }
}

int main(int argc, char* argv[]) {
    load_config(); 

    union REGS regs;
    regs.h.ah = 0x0F; // BIOS Get Video Mode
    int86(0x10, &regs, &regs);
    
    int is_mda = (regs.h.al == 0x07);
    
    if (is_mda) { 
        // Mode 0x07 is MDA (80x25 Monochrome)
        video_address = 0xB0000; 
        
        // Auto-lock the UI into strict Monochrome to prevent attribute glitching
        setting_color = 0; 
        setting_bg_color = 0; 
        setting_fg_color = 7;
    } else {
        // Mode 0x03 is standard VGA/EGA/CGA Color
        video_address = 0xB8000; 
    }
    
    // Let the engine dynamically find its own VSYNC pins!
    calibrate_vsync(is_mda);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-486") == 0 || strcmp(argv[i], "/486") == 0) global_is_486 = 1;
        else if (strcmp(argv[i], "-speaker") == 0 || strcmp(argv[i], "/speaker") == 0){ 
            global_is_pc_speaker = 1;
            config_is_pc_speaker = 1;
        }
        else if (strcmp(argv[i], "-rate") == 0 || strcmp(argv[i], "/rate") == 0) { if (i + 1 < argc) { custom_sample_rate = atoi(argv[i+1]); i++; } } 
        else { strncpy(app_filename, argv[i], 511); app_filename[511] = '\0'; has_active_file = 1; }
    }

    if (has_active_file && strcmp(app_filename, "No File") != 0) {
        start_off = 0; file_is_native_wav = 0; current_bitdepth = 16; wav_data_size = 0;
        current_sample_rate = parse_wav_header(app_filename, &start_off, &current_channels, &current_bitdepth, &wav_data_size);
        if (current_sample_rate > 0) {
            file_is_native_wav = 1; file_is_vbr = 0;
            file_bitrate_kbps = (current_sample_rate * current_channels * current_bitdepth) / 1000;
            file_size = start_off + wav_data_size;
            ui_total_seconds = wav_data_size / (current_channels * (current_bitdepth / 8) * current_sample_rate);
        } else {
            start_off = 0;
            current_sample_rate = parse_mp3_header(app_filename, &start_off, &current_channels);
            if (!current_sample_rate) { printf("Error: Invalid MP3 header.\n"); return 1; }
        }
    } else {
        strcpy(app_filename, "No File"); current_sample_rate = 44100; current_channels = 2; ui_view = 0; 
        getcwd(current_dir, 512); strcpy(dialog_address_text, current_dir); is_paused = 1; 
    }

    if (!global_is_pc_speaker) { 
        if (!reset_dsp()) { printf("Sound Blaster not detected. Falling back to PC speaker...\n"); delay(1500); global_is_pc_speaker = 1; } 
        else { get_dsp_version(&dsp_major, &dsp_minor); is_sb16 = (dsp_major >= 4); has_auto_init = (dsp_major > 2) || (dsp_major == 2 && dsp_minor >= 1); }
    }

    target_sample_rate = current_sample_rate; target_channels = current_channels;
    if (custom_channels > 0) target_channels = custom_channels;
    
    if (global_is_pc_speaker) {
        target_channels = 1; active_bitdepth = 8; active_buffer_size = custom_buffer_size > 0 ? custom_buffer_size : 8192; 
        target_sample_rate = custom_sample_rate > 0 ? custom_sample_rate : 22050; pit_divisor = 1193180 / target_sample_rate;
    } else {
        active_bitdepth = is_sb16 ? 16 : 8; active_buffer_size = custom_buffer_size > 0 ? custom_buffer_size : (is_sb16 ? 65536 : 32768); 
        if (global_is_486 && !file_is_native_wav){ 
            target_sample_rate = current_sample_rate / CONF_486_DIVISOR; 
            target_channels = CONF_486_CHANNELS; 
            if (custom_sample_rate > 0) target_sample_rate = custom_sample_rate; 
        } 
        else { if (custom_sample_rate > 0) target_sample_rate = custom_sample_rate; if (is_sb16 && target_sample_rate > 44100) target_sample_rate = 44100; }
        if (!is_sb16) {
            if (!has_auto_init) { target_channels = 1; if (target_sample_rate > 21739) target_sample_rate = 21739; } 
            else if (dsp_major == 2 && has_auto_init) { target_channels = 1; if (target_sample_rate > 44100) target_sample_rate = 44100; } 
            else if (dsp_major == 3) { if (target_channels == 2 && target_sample_rate > 22050) target_sample_rate = 22050; if (target_channels == 1 && target_sample_rate > 44100) target_sample_rate = 44100; }
            int output_rate = (target_channels == 2) ? target_sample_rate * 2 : target_sample_rate;
            int tc = 256 - ((1000000 + (output_rate / 2)) / output_rate); int actual_output_rate = 1000000 / (256 - tc); target_sample_rate = (target_channels == 2) ? (actual_output_rate / 2) : actual_output_rate;
        }
    }
    resample_step = ((unsigned long long)current_sample_rate << 16) / target_sample_rate; resample_pos = 0;
    
    if (has_active_file) {
        FILE *mp3_file = fopen(app_filename, "rb"); if (!mp3_file) return 1;
        fseek(mp3_file, 0, SEEK_END); file_size = ftell(mp3_file); if (start_off > file_size) start_off = 0;
        
        if (global_is_486 && !file_is_native_wav) { // <-- 1. BYPASS IF WAV!
            int conversion_success = 0, active_divisor = CONF_486_DIVISOR;
            while (!conversion_success && active_divisor <= 4) {
                if (!global_is_pc_speaker && custom_sample_rate == 0) { 
                    target_sample_rate = current_sample_rate / active_divisor; 
                    if (is_sb16 && target_sample_rate > 44100) target_sample_rate = 44100; 
                    
                    // --- APPLY SB PRO 1MHz HARDWARE LIMITS TO TRANSCODER ---
                    if (!is_sb16 && dsp_major == 3) {
                        if (target_channels == 2 && target_sample_rate > 22050) target_sample_rate = 22050;
                        if (target_channels == 1 && target_sample_rate > 44100) target_sample_rate = 44100;
                        int tc = 256 - (1000000 / (target_channels == 2 ? target_sample_rate * 2 : target_sample_rate));
                        target_sample_rate = (target_channels == 2) ? (1000000 / (256 - tc)) / 2 : (1000000 / (256 - tc));
                    }
                    // -------------------------------------------------------
                    
                    resample_step = ((unsigned long long)current_sample_rate << 16) / target_sample_rate; 
                }
                printf("\n[ 486 OPTIMIZATION: Transcoding MP3 to WAV (1/%d Quality) ]\nProcessing... ", active_divisor);
                FILE *wav_file = fopen("486_temp.wav", "wb+");
                if (wav_file) {
                    unsigned char dummy_header[44] = {0}; fwrite(dummy_header, 1, 44, wav_file);
                    mp3dec_t temp_mp3d; mp3dec_init(&temp_mp3d); mp3dec_frame_info_t temp_info; 
                    
                    active_audio_file = mp3_file; fseek(active_audio_file, start_off, SEEK_SET);
                    stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); current_ram_ptr = stream_buffer;
                    long trans_bytes_left = file_size - start_off; int total_wav_bytes = 0, disk_full = 0;
                    
                    while (trans_bytes_left > 0) {
                        refill_stream(); int avail = stream_bytes - (current_ram_ptr - stream_buffer);
                        int samples = mp3dec_decode_frame(&temp_mp3d, current_ram_ptr, avail, pcm_temp, &temp_info);
                        if (samples > 0) {
                            int out_samples = samples; short* final_pcm_ptr = pcm_temp;
                            if (current_sample_rate != target_sample_rate || current_channels != target_channels) {
                                out_samples = 0; unsigned int temp_resample_pos = 0;
                                while ((temp_resample_pos >> 16) < samples) {
                                    int src_idx = temp_resample_pos >> 16; unsigned int frac = temp_resample_pos & 0xFFFF;
                                    if (src_idx >= samples - 1) { if (target_channels == 2 && current_channels == 2) { resampled_temp[out_samples*2] = pcm_temp[src_idx*2]; resampled_temp[out_samples*2+1] = pcm_temp[src_idx*2+1]; } else if (target_channels == 1 && current_channels == 2) { resampled_temp[out_samples] = (pcm_temp[src_idx*2] >> 1) + (pcm_temp[src_idx*2+1] >> 1); } else if (target_channels == 2 && current_channels == 1) { resampled_temp[out_samples*2] = pcm_temp[src_idx]; resampled_temp[out_samples*2+1] = pcm_temp[src_idx]; } else resampled_temp[out_samples] = pcm_temp[src_idx]; } 
                                    else { if (target_channels == 2 && current_channels == 2) { int L0=pcm_temp[src_idx*2], L1=pcm_temp[src_idx*2+2], R0=pcm_temp[src_idx*2+1], R1=pcm_temp[src_idx*2+3]; resampled_temp[out_samples*2] = L0 + (((L1 - L0) * (int)frac) >> 16); resampled_temp[out_samples*2+1] = R0 + (((R1 - R0) * (int)frac) >> 16); } else if (target_channels == 1 && current_channels == 2) { int L0=pcm_temp[src_idx*2], L1=pcm_temp[src_idx*2+2], R0=pcm_temp[src_idx*2+1], R1=pcm_temp[src_idx*2+3]; int M0=(L0>>1)+(R0>>1), M1=(L1>>1)+(R1>>1); resampled_temp[out_samples] = M0 + (((M1 - M0) * (int)frac) >> 16); } else if (target_channels == 2 && current_channels == 1) { int S0=pcm_temp[src_idx], S1=pcm_temp[src_idx+1]; int val = S0 + (((S1 - S0) * (int)frac) >> 16); resampled_temp[out_samples*2] = val; resampled_temp[out_samples*2+1] = val; } else { int S0=pcm_temp[src_idx], S1=pcm_temp[src_idx+1]; resampled_temp[out_samples] = S0 + (((S1 - S0) * (int)frac) >> 16); } } out_samples++; temp_resample_pos += resample_step;
                                } final_pcm_ptr = resampled_temp;
                            }
                            int final_total_samples = out_samples * target_channels; int expected_bytes = 0, written_bytes = 0;
                            if (active_bitdepth == 8) { unsigned char out_8bit[MINIMP3_MAX_SAMPLES_PER_FRAME * 2]; for (int i = 0; i < final_total_samples; i++) out_8bit[i] = (unsigned char)((final_pcm_ptr[i] >> 8) + 128); expected_bytes = final_total_samples; written_bytes = fwrite(out_8bit, 1, expected_bytes, wav_file); } else { expected_bytes = final_total_samples * 2; written_bytes = fwrite(final_pcm_ptr, 1, expected_bytes, wav_file); }
                            if (written_bytes != expected_bytes) { disk_full = 1; break; } total_wav_bytes += written_bytes;
                        }
                        if (disk_full) break;
                        if (temp_info.frame_bytes > 0) { current_ram_ptr += temp_info.frame_bytes; trans_bytes_left -= temp_info.frame_bytes; } else { current_ram_ptr++; trans_bytes_left--; }
                    }
                    if (disk_full) { fclose(wav_file); remove("486_temp.wav"); active_divisor++; continue; }
                    rewind(wav_file); write_wav_header(wav_file, target_sample_rate, target_channels, active_bitdepth, total_wav_bytes);
                    global_wav_bytes = total_wav_bytes; ui_total_seconds = total_wav_bytes / (target_sample_rate * target_channels * (active_bitdepth / 8)); conversion_success = 1;
                    fclose(mp3_file); active_audio_file = wav_file; fseek(active_audio_file, 44, SEEK_SET);
                } else return 1;
                if (global_is_pc_speaker || custom_sample_rate > 0) break;
            }
            if (!conversion_success) { if(active_audio_file) fclose(active_audio_file); return 1; }
        } 
        else {
            active_audio_file = mp3_file; 
            fseek(active_audio_file, start_off, SEEK_SET);
            stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
            current_ram_ptr = stream_buffer;
            
            if (!file_is_native_wav) { // <-- 2. PROTECT THE MP3 METADATA PARSER!
                mp3dec_init(&mp3d); mp3dec_frame_info_t temp_info; mp3dec_decode_frame(&mp3d, current_ram_ptr, stream_bytes, pcm_temp, &temp_info); 
                int vbr_frames = 0; file_is_vbr = 0;
                for (int i = 0; i < 200 && i < stream_bytes - 12; i++) {
                    if ((current_ram_ptr[i] == 'X' && current_ram_ptr[i+1] == 'i' && current_ram_ptr[i+2] == 'n' && current_ram_ptr[i+3] == 'g') || (current_ram_ptr[i] == 'I' && current_ram_ptr[i+1] == 'n' && current_ram_ptr[i+2] == 'f' && current_ram_ptr[i+3] == 'o')) {
                        int flags = (current_ram_ptr[i+4] << 24) | (current_ram_ptr[i+5] << 16) | (current_ram_ptr[i+6] << 8) | current_ram_ptr[i+7];
                        if (flags & 1) { vbr_frames = (current_ram_ptr[i+8] << 24) | (current_ram_ptr[i+9] << 16) | (current_ram_ptr[i+10] << 8) | current_ram_ptr[i+11]; file_is_vbr = 1; } break;
                    }
                }
                if (vbr_frames > 0 && current_sample_rate > 0) { int samples_per_frame = (current_sample_rate >= 32000) ? 1152 : 576; long total_samples_vbr = (long)vbr_frames * samples_per_frame; ui_total_seconds = total_samples_vbr / current_sample_rate; file_bitrate_kbps = (file_size * 8) / (ui_total_seconds > 0 ? ui_total_seconds * 1000 : 1); } else if (temp_info.bitrate_kbps > 0) { ui_total_seconds = (file_size * 8) / (temp_info.bitrate_kbps * 1000); file_bitrate_kbps = temp_info.bitrate_kbps; }
                mp3dec_init(&mp3d);
            }
        }
    }
    
    if (has_active_file) { 
        //printf("\nPress ANY KEY to launch Player UI...\n"); 
    }
    if(init_audio_engine()) return 1;

    int keep_playing = 1, pcm_leftover_offset = 0, prev_mouse_left = 0, prev_mouse_right = 0;
    ui_total_samples_played = 0; 
    clock_t last_file_click_time = 0; 
    int last_clicked_file_idx = -1; 
    int advance_playlist = 0;

    while (keep_playing) {// Main Playback Loop
        if (advance_playlist) {
            advance_playlist = 0;
            if (queue_current >= 0 && queue_current < queue_count - 1) { 
                queue_current++; 
                load_new_file_from_browser(queue_paths[queue_current]); 
                continue; 
            } 
            else { 
                queue_current = -1;
                has_active_file = 0;
                if (active_audio_file) { fclose(active_audio_file); active_audio_file = NULL; }
                strcpy(app_filename, "No File");
                ui_total_seconds = 0;
                ui_total_samples_played = 0;
                is_paused = 1; // Pause the engine to save CPU
                
                if (has_mouse) hide_mouse();
                clear_inner_ui();
                force_ui_redraw = 1;
                init_ui(app_filename);
                if (has_mouse) show_mouse();
            }
        }

        if (ui_view != 2) settings_saved_flag = 0; 
        if (ui_view != 1) { 
            browser_enqueue_only = 0; 
            browser_save_mode = 0; 
        }

        if (refill_request > 0) {
            if (next_vu_left_peak > vu_left_peak) vu_left_peak = next_vu_left_peak; if (next_vu_right_peak > vu_right_peak) vu_right_peak = next_vu_right_peak; next_vu_left_peak = 0; next_vu_right_peak = 0;
            for (int i = 0; i < NUM_BANDS; i++) { 
                if (next_spectrum_peaks[i] > spectrum_peaks[i]) spectrum_peaks[i] = next_spectrum_peaks[i]; 
                next_spectrum_peaks[i] = 0; 
            }
            int half = refill_request - 1; 
            refill_request = 0; 
            int fill_idx = half * (active_buffer_size / 2); 
            int end_idx = fill_idx + (active_buffer_size / 2);

            while (fill_idx < end_idx && keep_playing) {
                if (is_paused || !has_active_file || !active_audio_file) {
                    unsigned char zero[2048]; 
                    memset(zero, (active_bitdepth == 8) ? 128 : 0, 2048); 
                    int fill_len = end_idx - fill_idx; if (fill_len > 2048) fill_len = 2048;
                    if (global_is_pc_speaker) memcpy(pc_speaker_dma + fill_idx, zero, fill_len); 
                    else dosmemput(zero, fill_len, physical_addr + fill_idx); fill_idx += fill_len; 
                    continue;
                }
                if (global_is_486) {
                    int bytes_to_copy = end_idx - fill_idx;
                    if (bytes_to_copy > 0) {
                        // --- FIX: READ INTO THE MASSIVE 64KB STREAM BUFFER, NOT PCM_TEMP! ---
                        int read_bytes = fread(stream_buffer, 1, bytes_to_copy, active_audio_file);
                        if (read_bytes <= 0) { if (is_looping) { fseek(active_audio_file, 44, SEEK_SET); ui_total_samples_played = 0; continue; } else { advance_playlist = 1; break; } }
                        frames_decoded++; short* wav_pcm = (short*)stream_buffer; int num_samples = read_bytes / (target_channels * (active_bitdepth / 8)); 
                        
                        if (visualizer_mode == 2 && ui_view == 0) { long sum_l = 0, sum_r = 0; int count = 0; if (active_bitdepth == 16) { for (int i = 0; i < num_samples; i += 4) { sum_l += abs(wav_pcm[i * target_channels]); if (target_channels == 2) sum_r += abs(wav_pcm[i * target_channels + 1]); count++; } } else { unsigned char* wav_pcm_8 = (unsigned char*)stream_buffer; for (int i = 0; i < num_samples; i += 4) { sum_l += abs((int)wav_pcm_8[i * target_channels] - 128); if (target_channels == 2) sum_r += abs((int)wav_pcm_8[i * target_channels + 1] - 128); count++; } } if (count > 0) { long l = ((sum_l / count) * 5) / 2; l *= VISUALIZER_GLOBAL_BOOST; if (active_bitdepth == 8) l *= 256; if (l > 32767) l = 32767; if (l > next_vu_left_peak) next_vu_left_peak = l; if (target_channels == 2) { long r = ((sum_r / count) * 5) / 2; r *= VISUALIZER_GLOBAL_BOOST; if (active_bitdepth == 8) r *= 256; if (r > 32767) r = 32767; if (r > next_vu_right_peak) next_vu_right_peak = r; } else { next_vu_right_peak = next_vu_left_peak; } } } 
                        else if (visualizer_mode == 1 && ui_view == 0) { int freqs[NUM_BANDS] = {73, 110, 176, 294, 441, 735, 1102, 2205, 4410, 8820}; int boosts[NUM_BANDS] = {1, 2, 2, 3, 4, 5, 7, 10, 15, 22}; int max_limit = num_samples; if (max_limit > 1024) max_limit = 1024; for (int b = 0; b < NUM_BANDS; b++) { long real_part = 0, imag_part = 0; int p = target_sample_rate / freqs[b]; p = (p / 4) * 4; if (p < 4) p = 4; int hp = p / 2, qp = p / 4; int actual_limit = max_limit - (max_limit % p); if (actual_limit < p) actual_limit = p; int loop_count = actual_limit / 2; if (loop_count == 0) loop_count = 1; for (int i = 0; i < actual_limit; i += 2) { int val = 0; if (active_bitdepth == 16) val = wav_pcm[i * target_channels]; else val = ((int)((unsigned char*)stream_buffer)[i * target_channels] - 128) * 256; int phase = i % p; if (phase < hp) real_part += val; else real_part -= val; int phase2 = (phase + qp) % p; if (phase2 < hp) imag_part += val; else imag_part -= val; } long amp = (abs(real_part) + abs(imag_part)) / loop_count; amp *= boosts[b]; amp *= VISUALIZER_GLOBAL_BOOST; if (amp > 32767) amp = 32767; if (amp > next_spectrum_peaks[b]) next_spectrum_peaks[b] = amp; } }
                        
                        if (global_is_pc_speaker) memcpy(pc_speaker_dma + fill_idx, stream_buffer, read_bytes); 
                        else dosmemput(stream_buffer, read_bytes, physical_addr + fill_idx); 
                        fill_idx += read_bytes; ui_total_samples_played += num_samples; 
                    }
                } 
                else {
                    if (pcm_leftover_bytes > 0) { int copy = pcm_leftover_bytes; if (fill_idx + copy > end_idx) copy = end_idx - fill_idx; if (global_is_pc_speaker) memcpy(pc_speaker_dma + fill_idx, (unsigned char*)pcm_temp + pcm_leftover_offset, copy); else dosmemput((unsigned char*)pcm_temp + pcm_leftover_offset, copy, physical_addr + fill_idx); fill_idx += copy; pcm_leftover_offset += copy; pcm_leftover_bytes -= copy; } 
                    else {
                        refill_stream(); int avail = stream_bytes - (current_ram_ptr - stream_buffer);
                        
                        // --- FIX: EOF INFINITE LOOP AVOIDANCE ---
                        int min_bytes = file_is_native_wav ? (current_channels * (current_bitdepth / 8)) : 1;
                        if (avail < min_bytes && feof(active_audio_file)) { 
                            if (is_looping) { fseek(active_audio_file, start_off, SEEK_SET); stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); current_ram_ptr = stream_buffer; resample_pos = 0; ui_total_samples_played = 0; continue; } 
                            else { advance_playlist = 1; break; } 
                        }
                        // ----------------------------------------
                        
                        // --- NEW: DUAL MP3 / NATIVE WAV REAL-TIME STREAMING! ---
                        int samples = 0;
                        if (file_is_native_wav) {
                            int bytes_per_sample = current_channels * (current_bitdepth / 8);
                            int frames_to_read = 1152;
                            int bytes_to_read = frames_to_read * bytes_per_sample;
                            if (bytes_to_read > avail) bytes_to_read = avail - (avail % bytes_per_sample);
                            
                            if (bytes_to_read > 0) {
                                if (current_bitdepth == 8) {
                                    unsigned char* wav8 = (unsigned char*)current_ram_ptr;
                                    for (int i=0; i < (bytes_to_read / bytes_per_sample) * current_channels; i++) {
                                        pcm_temp[i] = (wav8[i] - 128) << 8;
                                    }
                                } else {
                                    memcpy(pcm_temp, current_ram_ptr, bytes_to_read);
                                }
                                current_ram_ptr += bytes_to_read;
                                samples = bytes_to_read / bytes_per_sample;
                            }
                        } else {
                            samples = mp3dec_decode_frame(&mp3d, current_ram_ptr, avail, pcm_temp, &info);
                            if (info.frame_bytes > 0) current_ram_ptr += info.frame_bytes; else current_ram_ptr++;
                        }
                        // -------------------------------------------------------

                        if (samples > 0) {
                            frames_decoded++; int out_samples = samples; short* final_pcm_ptr = pcm_temp;
                            if (current_sample_rate != target_sample_rate || current_channels != target_channels) { out_samples = 0; while ((resample_pos >> 16) < samples) { int src_idx = resample_pos >> 16; unsigned int frac = resample_pos & 0xFFFF; if (src_idx >= samples - 1) { if (target_channels == 2 && current_channels == 2) { resampled_temp[out_samples*2] = pcm_temp[src_idx*2]; resampled_temp[out_samples*2+1] = pcm_temp[src_idx*2+1]; } else if (target_channels == 1 && current_channels == 2) { resampled_temp[out_samples] = (pcm_temp[src_idx*2] >> 1) + (pcm_temp[src_idx*2+1] >> 1); } else if (target_channels == 2 && current_channels == 1) { resampled_temp[out_samples*2] = pcm_temp[src_idx]; resampled_temp[out_samples*2+1] = pcm_temp[src_idx]; } else { resampled_temp[out_samples] = pcm_temp[src_idx]; } } else { if (target_channels == 2 && current_channels == 2) { int L0=pcm_temp[src_idx*2], L1=pcm_temp[src_idx*2+2], R0=pcm_temp[src_idx*2+1], R1=pcm_temp[src_idx*2+3]; resampled_temp[out_samples*2] = L0 + (((L1 - L0) * (int)frac) >> 16); resampled_temp[out_samples*2+1] = R0 + (((R1 - R0) * (int)frac) >> 16); } else if (target_channels == 1 && current_channels == 2) { int L0=pcm_temp[src_idx*2], L1=pcm_temp[src_idx*2+2], R0=pcm_temp[src_idx*2+1], R1=pcm_temp[src_idx*2+3]; int M0=(L0>>1)+(R0>>1), M1=(L1>>1)+(R1>>1); resampled_temp[out_samples] = M0 + (((M1 - M0) * (int)frac) >> 16); } else if (target_channels == 2 && current_channels == 1) { int S0=pcm_temp[src_idx], S1=pcm_temp[src_idx+1]; int val = S0 + (((S1 - S0) * (int)frac) >> 16); resampled_temp[out_samples*2] = val; resampled_temp[out_samples*2+1] = val; } else { int S0=pcm_temp[src_idx], S1=pcm_temp[src_idx+1]; resampled_temp[out_samples] = S0 + (((S1 - S0) * (int)frac) >> 16); } } out_samples++; resample_pos += resample_step; } resample_pos -= (samples << 16); final_pcm_ptr = resampled_temp; }
                            if (out_samples > 0) { if (visualizer_mode == 2 && ui_view == 0) { long sum_l = 0, sum_r = 0; int count = 0; for (int i = 0; i < out_samples; i += 4) { sum_l += abs(final_pcm_ptr[i * target_channels]); if (target_channels == 2) sum_r += abs(final_pcm_ptr[i * target_channels + 1]); count++; } if (count > 0) { long l = ((sum_l / count) * 5) / 2; l *= VISUALIZER_GLOBAL_BOOST; if (l > 32767) l = 32767; if (l > next_vu_left_peak) next_vu_left_peak = l; if (target_channels == 2) { long r = ((sum_r / count) * 5) / 2; r *= VISUALIZER_GLOBAL_BOOST; if (active_bitdepth == 8) r *= 256; if (r > 32767) r = 32767; if (r > next_vu_right_peak) next_vu_right_peak = r; } else next_vu_right_peak = next_vu_left_peak; } } else if (visualizer_mode == 1 && ui_view == 0) { int freqs[NUM_BANDS] = {73, 110, 176, 294, 441, 735, 1102, 2205, 4410, 8820}; int boosts[NUM_BANDS] = {1, 2, 2, 3, 4, 5, 7, 10, 15, 22}; int max_limit = out_samples; if (max_limit > 1024) max_limit = 1024; for (int b = 0; b < NUM_BANDS; b++) { long real_part = 0, imag_part = 0; int p = target_sample_rate / freqs[b]; p = (p / 4) * 4; if (p < 4) p = 4; int hp = p / 2, qp = p / 4; int actual_limit = max_limit - (max_limit % p); if (actual_limit < p) actual_limit = p; int loop_count = actual_limit / 2; if (loop_count == 0) loop_count = 1; for (int i = 0; i < actual_limit; i += 2) { int val = final_pcm_ptr[i * target_channels]; int phase = i % p; if (phase < hp) real_part += val; else real_part -= val; int phase2 = (phase + qp) % p; if (phase2 < hp) imag_part += val; else imag_part -= val; } long amp = (abs(real_part) + abs(imag_part)) / loop_count; amp *= boosts[b]; amp *= VISUALIZER_GLOBAL_BOOST; if (amp > 32767) amp = 32767; if (amp > next_spectrum_peaks[b]) next_spectrum_peaks[b] = amp; } } }
                            int final_total_samples = out_samples * target_channels; if (active_bitdepth == 8) { unsigned char* out_8bit = (unsigned char*)pcm_temp; for (int i = 0; i < final_total_samples; i++) out_8bit[i] = (unsigned char)((final_pcm_ptr[i] >> 8) + 128); pcm_leftover_bytes = final_total_samples; } else { if (final_pcm_ptr != pcm_temp) memcpy(pcm_temp, final_pcm_ptr, final_total_samples * 2); pcm_leftover_bytes = final_total_samples * 2; } pcm_leftover_offset = 0; ui_total_samples_played += out_samples; 
                        }
                    }
                } 
            }
        } 
        else { 
            wait_vsync(); 
        }
        
        int ui_current_sec = target_sample_rate > 0 ? ui_total_samples_played / target_sample_rate : 0;
        update_ui(ui_current_sec, ui_total_seconds); 
        update_mouse(); 
        
        if (kbhit()) { //keyboard handler
            int c = getch(); 
            int ext = 0; 
            if (c == 0 || c == 224) { 
                ext = getch();
                if (active_menu == 0 && ui_view == 0) { if (ext == 72) c = '+'; else if (ext == 80) c = '-'; else if (ext == 77) c = 'f'; else if (ext == 75) c = 'b'; }
                else if (ui_view == 1) {
                    if (active_input_field == 2) { 
                        if (ext == 75) { if (active_input_field == 2 && dialog_search_cursor > 0) dialog_search_cursor--; else if (active_input_field == 3 && dialog_address_cursor > 0) dialog_address_cursor--; else if (active_input_field == 4 && dialog_filename_cursor > 0) dialog_filename_cursor--; }
                        else if (ext == 77) { if (active_input_field == 2 && dialog_search_cursor < (int)strlen(dialog_search_text)) dialog_search_cursor++; else if (active_input_field == 3 && dialog_address_cursor < (int)strlen(dialog_address_text)) dialog_address_cursor++; else if (active_input_field == 4 && dialog_filename_cursor < (int)strlen(dialog_file_name)) dialog_filename_cursor++; }
                        else if (ext == 83) { if (active_input_field == 2 && dialog_search_cursor < (int)strlen(dialog_search_text)) { int len = (int)strlen(dialog_search_text); memmove(&dialog_search_text[dialog_search_cursor], &dialog_search_text[dialog_search_cursor + 1], len - dialog_search_cursor); filter_browser_files(); } else if (active_input_field == 3 && dialog_address_cursor < (int)strlen(dialog_address_text)) { int len = (int)strlen(dialog_address_text); memmove(&dialog_address_text[dialog_address_cursor], &dialog_address_text[dialog_address_cursor + 1], len - dialog_address_cursor); browser_needs_redraw = 1; } else if (active_input_field == 4 && dialog_filename_cursor < (int)strlen(dialog_file_name)) { int len = (int)strlen(dialog_file_name); memmove(&dialog_file_name[dialog_filename_cursor], &dialog_file_name[dialog_filename_cursor + 1], len - dialog_filename_cursor); browser_needs_redraw = 1; } }
                    } else if (active_tab_element == 3) {
                        if (ext == 72) { if (file_selected > 0) { file_selected--; if (file_selected < file_scroll) file_scroll = file_selected; if (!file_list[file_selected].is_dir) { strcpy(dialog_file_name, file_list[file_selected].name); dialog_filename_cursor = strlen(dialog_file_name); } browser_needs_redraw = 1; } }
                        else if (ext == 80) { if (file_selected < file_count - 1) { file_selected++; if (file_selected >= file_scroll + 9) file_scroll = file_selected - 8; if (!file_list[file_selected].is_dir) { strcpy(dialog_file_name, file_list[file_selected].name); dialog_filename_cursor = strlen(dialog_file_name); } browser_needs_redraw = 1; } }
                        c = 0;
                    } else c = 0;
                }
                else if (ui_view == 3) { 
                    if (ext == 72) { if (queue_selected == -1 && queue_count > 0) { queue_selected = queue_scroll; } else if (queue_selected > 0) { queue_selected--; if (queue_selected < queue_scroll) queue_scroll = queue_selected; } browser_needs_redraw = 1; } 
                    else if (ext == 80) { if (queue_selected == -1 && queue_count > 0) { queue_selected = queue_scroll; } else if (queue_selected < queue_count - 1) { queue_selected++; if (queue_selected >= queue_scroll + 11) queue_scroll = queue_selected - 10; } browser_needs_redraw = 1; }
                    else if (ext == 83) { if (queue_selected >= 0 && queue_selected < queue_count) { for (int i = queue_selected; i < queue_count - 1; i++) { strcpy(queue_paths[i], queue_paths[i+1]); strcpy(queue_displays[i], queue_displays[i+1]); } queue_count--; if (queue_current == queue_selected) { advance_playlist = 1; queue_current = -1; } else if (queue_current > queue_selected) { queue_current--; } if (queue_selected >= queue_count) queue_selected = queue_count - 1; if (queue_scroll > 0 && queue_scroll > queue_count - 11) queue_scroll--; browser_needs_redraw = 1; } }
                    c = 0;
                } 
                else if (ui_view == 4) {
                    if (ext == 72) { if (info_scroll > 0) { info_scroll--; force_ui_redraw = 1; } } 
                    else if (ext == 80) { info_scroll++; force_ui_redraw = 1; }
                    c = 0;
                }
                else c = 0;
            }

            if (c == 17 && !ext) { keep_playing = 0; }
            else if (c == 15 && !ext) { int shift_pressed = (_farpeekb(_dos_ds, 0x417) & 0x03); if (has_mouse) hide_mouse(); active_menu = 0; active_menu_hover = -1; ui_view = 1; browser_needs_redraw = 1; force_ui_redraw = 1; active_tab_element = 0; active_input_field = 0; getcwd(current_dir, 512); load_dir(current_dir, 1); if (shift_pressed) { strcpy(dialog_search_text, ".m3u"); dialog_search_cursor = strlen(dialog_search_text); filter_browser_files(); } clear_inner_ui(); if (has_mouse) show_mouse(); c = 0; }
            else if (ext == 75 && (_farpeekb(_dos_ds, 0x417) & 0x03)) { if (queue_count > 0 && queue_current > 0) { queue_current--; load_new_file_from_browser(queue_paths[queue_current]); } c = 0; ext = 0; }
            else if (ext == 77 && (_farpeekb(_dos_ds, 0x417) & 0x03)) { if (queue_count > 0 && queue_current < queue_count - 1) { queue_current++; load_new_file_from_browser(queue_paths[queue_current]); } c = 0; ext = 0; }
            else if (ext == 34) { if (has_active_file) is_paused = !is_paused; c = 0; ext = 0; }
            else if (ext == 36) { if (has_active_file) { if (global_is_486) fseek(active_audio_file, 44, SEEK_SET); else { fseek(active_audio_file, start_off, SEEK_SET); stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); current_ram_ptr = stream_buffer; } ui_total_samples_played = 0; is_paused = 1; resample_pos = 0; pcm_leftover_bytes = 0; } c = 0; ext = 0; }
            else if (ext == 16) { if (queue_count > 0 && queue_current > 0) { queue_current--; load_new_file_from_browser(queue_paths[queue_current]); } c = 0; ext = 0; }
            else if (ext == 25) { if (queue_count > 0 && queue_current < queue_count - 1) { queue_current++; load_new_file_from_browser(queue_paths[queue_current]); } c = 0; ext = 0; }
            else if (ext == 46) { set_volume(master_volume - 5); c = 0; ext = 0; }
            else if (ext == 48) { set_volume(master_volume + 5); c = 0; ext = 0; }

            if (active_menu != 0) { if (c == 27) { active_menu = 0; active_menu_hover = -1; if (has_mouse) hide_mouse(); force_ui_redraw = 1; browser_needs_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse(); } } 
            else if (ui_view == 0 || ui_view == 3 || ui_view == 4) {
                if (c == 27 || c == 'q' || c == 'Q') keep_playing = 0;
                if (c == '+' || c == '=') { set_volume(master_volume + 5); } 
                if (c == '-' || c == '_') { set_volume(master_volume - 5); } 
                if (c == ' ') { if (has_active_file) { is_paused = !is_paused; } } 
                if (c == 'l' || c == 'L') { if (has_active_file) { is_looping = !is_looping; } } 
                if (c == 'x' || c == 'X') { if (has_active_file) { if (global_is_486) fseek(active_audio_file, 44, SEEK_SET); else { fseek(active_audio_file, start_off, SEEK_SET); stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); current_ram_ptr = stream_buffer; } ui_total_samples_played = 0; is_paused = 1; resample_pos = 0; pcm_leftover_bytes = 0; } } 
                if (c == 'f' || c == 'F') {
                    if (has_active_file && active_audio_file) {
                        float pct = (float)(ui_current_sec + 5) / (float)ui_total_seconds;
                        if (pct > 1.0f) pct = 1.0f; if (global_is_486) { 
                            long target_byte = (long)(global_wav_bytes * pct); 
                            int frame_size = target_channels * (active_bitdepth / 8); 
                            target_byte -= (target_byte % frame_size); 
                            fseek(active_audio_file, 44 + target_byte, SEEK_SET); 
                        } else { 
                            long track_len = file_is_native_wav ? wav_data_size : (file_size - start_off); 
                            long target_byte = (long)(track_len * pct); 
                            
                            if (file_is_native_wav) {
                                int bps = current_channels * (current_bitdepth / 8);
                                target_byte -= (target_byte % bps); // Safe memory alignment!
                            }
                            
                            fseek(active_audio_file, start_off + target_byte, SEEK_SET); 
                            stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
                            current_ram_ptr = stream_buffer; 
                            if (!file_is_native_wav) mp3dec_init(&mp3d); 
                        }
                        ui_total_samples_played = (unsigned long)((float)ui_total_seconds * target_sample_rate * pct);
                        resample_pos = 0; pcm_leftover_bytes = 0; force_ui_redraw = 1; 
                    }
                } 
                if (c == 'b' || c == 'B') { 
                    if (has_active_file && active_audio_file) { 
                        float pct = (float)(ui_current_sec - 5) / (float)ui_total_seconds; 
                        if (pct < 0.0f) pct = 0.0f; if (global_is_486) { 
                            long target_byte = (long)(global_wav_bytes * pct); 
                            int frame_size = target_channels * (active_bitdepth / 8); 
                            target_byte -= (target_byte % frame_size); 
                            fseek(active_audio_file, 44 + target_byte, SEEK_SET); 
                        } 
                        else { 
                            long track_len = file_is_native_wav ? wav_data_size : (file_size - start_off); 
                            long target_byte = (long)(track_len * pct); 
                            
                            if (file_is_native_wav) {
                                int bps = current_channels * (current_bitdepth / 8);
                                target_byte -= (target_byte % bps); // Safe memory alignment!
                            }
                            
                            fseek(active_audio_file, start_off + target_byte, SEEK_SET); 
                            stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
                            current_ram_ptr = stream_buffer; 
                            if (!file_is_native_wav) mp3dec_init(&mp3d); 
                        }
                        ui_total_samples_played = (unsigned long)((float)ui_total_seconds * target_sample_rate * pct);
                        resample_pos = 0; pcm_leftover_bytes = 0; force_ui_redraw = 1; 
                    }
                } 
                if (c == 13 && ui_view == 3 && queue_selected >= 0 && queue_selected < queue_count) { load_new_file_from_browser(queue_paths[queue_selected]); }
            } else if (ui_view == 1 || ui_view == 2) {
                if (c == 27 && !ext) { if (has_mouse) hide_mouse(); ui_view = 0; force_ui_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse(); } 
                else if (c == 9 && !ext) { if (ui_view == 1) { active_tab_element++; if (active_tab_element > 10) active_tab_element = 1; if (active_tab_element == 1) active_input_field = 3; else if (active_tab_element == 2) active_input_field = 2; else if (active_tab_element == 4) active_input_field = 4; else active_input_field = 0; if (active_tab_element == 3 && file_selected == -1 && file_count > 0) { file_selected = file_scroll; } browser_needs_redraw = 1; } } 
                else if (ui_view == 1 && c != 0) {
                    if (c == 13 && !ext && active_input_field == 0) { if (active_tab_element == 5) { execute_browser_ok(); } else if (active_tab_element == 6 || active_tab_element == 10) { if (has_mouse) hide_mouse(); ui_view = 0; force_ui_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse(); } else if (active_tab_element == 7) { if (path_history_index > 0) { path_history_index--; strcpy(browser_status_msg, ""); load_dir(path_history[path_history_index], 0); } } else if (active_tab_element == 8) { if (path_history_index < path_history_count - 1) { path_history_index++; strcpy(browser_status_msg, ""); load_dir(path_history[path_history_index], 0); } } else if (active_tab_element == 9) { char new_path[512]; int len = (int)strlen(current_dir); if (len > 3) { len -= 2; while (len > 0 && current_dir[len] != '\\' && current_dir[len] != '/') len--; strncpy(new_path, current_dir, len + 1); new_path[len + 1] = '\0'; strcpy(browser_status_msg, ""); load_dir(new_path, 1); } } else if (active_tab_element == 3 && file_selected >= 0 && file_selected < file_count) { strcpy(dialog_file_name, file_list[file_selected].name); execute_browser_ok(); } }
                    if (active_input_field == 2) { if (c == 8 && dialog_search_cursor > 0) { int len = (int)strlen(dialog_search_text); memmove(&dialog_search_text[dialog_search_cursor - 1], &dialog_search_text[dialog_search_cursor], len - dialog_search_cursor + 1); dialog_search_cursor--; filter_browser_files(); } else if (c >= 32 && c <= 126 && (int)strlen(dialog_search_text) < 12) { int len = (int)strlen(dialog_search_text); memmove(&dialog_search_text[dialog_search_cursor + 1], &dialog_search_text[dialog_search_cursor], len - dialog_search_cursor + 1); dialog_search_text[dialog_search_cursor] = (char)c; dialog_search_cursor++; filter_browser_files(); } }
                    else if (active_input_field == 3) { if (c == 8 && dialog_address_cursor > 0) { int len = (int)strlen(dialog_address_text); memmove(&dialog_address_text[dialog_address_cursor - 1], &dialog_address_text[dialog_address_cursor], len - dialog_address_cursor + 1); dialog_address_cursor--; browser_needs_redraw = 1; } else if (c == 13) { if (access(dialog_address_text, 0) == 0) { strcpy(current_dir, dialog_address_text); strcpy(browser_status_msg, ""); load_dir(current_dir, 1); } else { strcpy(browser_status_msg, "Directory doesnt exist"); } browser_needs_redraw = 1; } else if (c >= 32 && c <= 126 && (int)strlen(dialog_address_text) < 510) { int len = (int)strlen(dialog_address_text); memmove(&dialog_address_text[dialog_address_cursor + 1], &dialog_address_text[dialog_address_cursor], len - dialog_address_cursor + 1); dialog_address_text[dialog_address_cursor] = (char)c; dialog_address_cursor++; browser_needs_redraw = 1; } }
                    else if (active_input_field == 4) { if (c == 8 && dialog_filename_cursor > 0) { int len = (int)strlen(dialog_file_name); memmove(&dialog_file_name[dialog_filename_cursor - 1], &dialog_file_name[dialog_filename_cursor], len - dialog_filename_cursor + 1); dialog_filename_cursor--; browser_needs_redraw = 1; } else if (c == 13) { execute_browser_ok(); } else if (c >= 32 && c <= 126 && (int)strlen(dialog_file_name) < 254) { int len = (int)strlen(dialog_file_name); memmove(&dialog_file_name[dialog_filename_cursor + 1], &dialog_file_name[dialog_filename_cursor], len - dialog_filename_cursor + 1); dialog_file_name[dialog_filename_cursor] = (char)c; dialog_filename_cursor++; browser_needs_redraw = 1; } }
                }
            }
        }

        if (mouse_left && !prev_mouse_left) { //mouse click handler, mouse handler, click handler
            if (active_menu != 0) {
                if (active_menu == 1 && active_menu_hover != -1) { 
                    if (active_menu_hover == 0) { 
                        if (has_mouse) hide_mouse(); 
                        ui_view = 1; 
                        active_tab_element = 0; 
                        active_input_field = 0; 
                        browser_enqueue_only = 0; 
                        getcwd(current_dir, 512); 
                        load_dir(current_dir, 1); 
                        clear_inner_ui(); 
                        if (has_mouse) show_mouse(); 
                    } 
                    else if (active_menu_hover == 1) { 
                    if (has_mouse) hide_mouse(); 
                    ui_view = 1; 
                    active_tab_element = 0; 
                    active_input_field = 0; 
                    getcwd(current_dir, 512); 
                    load_dir(current_dir, 1); 
                    
                    // --- AUTO-FILTER FOR PLAYLISTS ---
                    strcpy(dialog_search_text, ".m3u"); 
                    dialog_search_cursor = strlen(dialog_search_text); 
                    filter_browser_files(); 
                    // ---------------------------------
                    
                    clear_inner_ui(); 
                    if (has_mouse) show_mouse(); 
                } 
                else if (active_menu_hover == 2) { 
                    if (has_mouse) hide_mouse(); 
                    ui_view = 4; 
                    info_scroll = 0; 
                    browser_needs_redraw = 1; 
                    force_ui_redraw = 1; 
                    init_ui(app_filename); 
                    if (has_mouse) show_mouse(); 
                } 
                else if (active_menu_hover == 3) { if (has_mouse) hide_mouse(); ui_view = 2; browser_needs_redraw = 1; force_ui_redraw = 1; if (has_mouse) show_mouse(); } else if (active_menu_hover == 4) { keep_playing = 0; } }
                else if (active_menu == 2 && active_menu_hover != -1) { 
                    if (active_menu_hover == 0) { 
                        if (config_is_pc_speaker != 0) { 
                            config_is_pc_speaker = 0; 
                        } 
                    } 
                    else if (active_menu_hover == 1) { 
                        if (config_is_pc_speaker != 1) { 
                            config_is_pc_speaker = 1; 
                        } 
                    } 
                    else if (active_menu_hover == 2) { 
                        set_volume(master_volume + 5); 
                    } 
                    else if (active_menu_hover == 3) { 
                        set_volume(master_volume - 5); 
                    } 
                    else if (active_menu_hover == 4) { 
                        if (has_active_file) { 
                            is_looping = !is_looping; 
                        } 
                    } 
                    else if (active_menu_hover == 5) { 
                        if (has_mouse) { 
                            hide_mouse(); 
                        } 
                        ui_view = 2; 
                        current_settings_tab = 3; 
                        browser_needs_redraw = 1; 
                        force_ui_redraw = 1; 
                        if (has_mouse) { 
                            show_mouse(); 
                        } 
                    } 
                }
                else if (active_menu == 3 && active_menu_hover != -1) { 
                    if (active_menu_hover >= 0 && active_menu_hover <= 2) { 
                        if (active_menu_hover == 0) { if (visualizer_mode == 2) visualizer_mode = 0; else visualizer_mode = 2; } 
                        else if (active_menu_hover == 1) { if (visualizer_mode == 1) visualizer_mode = 0; else visualizer_mode = 1; } 
                        else if (active_menu_hover == 2) { if (visualizer_mode == 3) visualizer_mode = 0; else visualizer_mode = 3; }
                        
                        if (ui_view == 0) { 
                            if (has_mouse) { hide_mouse(); } 
                            clear_inner_ui(); force_ui_redraw = 1; 
                            if (has_mouse) { show_mouse(); } 
                        } 
                    } else if (active_menu_hover == 3) { 
                        if (has_mouse) { hide_mouse(); } 
                        ui_view = 2; current_settings_tab = 4; browser_needs_redraw = 1; force_ui_redraw = 1; 
                        if (has_mouse) { show_mouse(); }
                    }
                }
                active_menu = 0; active_menu_hover = -1; if (has_mouse) hide_mouse(); force_ui_redraw = 1; browser_needs_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse();
            } // <--- THIS CLOSES THE DROP-DOWN MENU LOGIC!
            
            // --- START OF MAIN SCREEN CLICK LOGIC ---
            else if (ui_view == 0 || ui_view == 4) {
                 // --- TOP MENU BAR CLICKS ---
                if (mouse_y == 1) { 
                    if (mouse_x >= 1 && mouse_x <= 6) active_menu = 1; 
                    else if (mouse_x >= 8 && mouse_x <= 14) active_menu = 2; 
                    else if (mouse_x >= 16 && mouse_x <= 23) active_menu = 3; 
                    else if (mouse_x >= 75 && mouse_x <= 78) keep_playing = 0; 
                } 
                if (ui_view == 4 && mouse_y == 3 && mouse_x >= 69 && mouse_x <= 76) { if (has_mouse) hide_mouse(); ui_view = 0; force_ui_redraw = 1; browser_needs_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse(); }
                else if (ui_view == 4 && mouse_y == 3 && mouse_x == 78) { if (info_scroll > 0) { info_scroll--; force_ui_redraw = 1; } }
                else if (ui_view == 4 && mouse_y == 17 && mouse_x == 78) { info_scroll++; force_ui_redraw = 1; }
                
                // --- PROGRESS BAR SEEKING ---
                else if (mouse_y == 19 && mouse_x >= 9 && mouse_x <= 70) { 
                    if (has_active_file && active_audio_file && ui_total_seconds > 0) { 
                        float pct = (float)(mouse_x - 9) / 61.0f; 
                        if (pct < 0.0f) pct = 0.0f; if (pct > 1.0f) pct = 1.0f; 
                        if (global_is_486) { 
                            long target_byte = (long)(global_wav_bytes * pct); 
                            int frame_size = target_channels * (active_bitdepth / 8); 
                            target_byte -= (target_byte % frame_size); fseek(active_audio_file, 44 + target_byte, SEEK_SET); 
                        } else { 
                            long track_len = file_is_native_wav ? wav_data_size : (file_size - start_off); 
                            long target_byte = (long)(track_len * pct); 
                            if (file_is_native_wav) {
                                int bps = current_channels * (current_bitdepth / 8);
                                target_byte -= (target_byte % bps); 
                            }
                            fseek(active_audio_file, start_off + target_byte, SEEK_SET); 
                            stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
                            current_ram_ptr = stream_buffer; 
                            if (!file_is_native_wav) mp3dec_init(&mp3d); 
                        } 
                        ui_total_samples_played = (unsigned long)((float)ui_total_seconds * target_sample_rate * pct); 
                        resample_pos = 0; pcm_leftover_bytes = 0; force_ui_redraw = 1; 
                    } 
                }
                
                // --- BOTTOM BUTTONS LOGIC ---
                else if (mouse_y == 22) { 
                    if (mouse_x >= 2 && mouse_x <= 6) { //playlist button
                        if (has_mouse) hide_mouse(); 
                        ui_view = 3; 
                        browser_needs_redraw = 1; 
                        force_ui_redraw = 1; 
                        browser_enqueue_only = 0; 
                        init_ui(app_filename); 
                        if (has_mouse) show_mouse(); 
                    } else if (mouse_x >= 8 && mouse_x <= 12) { //settings button
                        if (has_mouse) hide_mouse(); 
                        ui_view = 2; 
                        browser_needs_redraw = 1; 
                        force_ui_redraw = 1; 
                        if (has_mouse) show_mouse(); 
                    } else if (mouse_x >= 20 && mouse_x <= 24) { //stop button
                        if (has_active_file && active_audio_file) { 
                            if (global_is_486) fseek(active_audio_file, 44, SEEK_SET); 
                            else { 
                                fseek(active_audio_file, start_off, SEEK_SET); 
                                stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
                                current_ram_ptr = stream_buffer; 
                            } 
                            ui_total_samples_played = 0; 
                            is_paused = 1; 
                            resample_pos = 0; 
                            pcm_leftover_bytes = 0; 
                        } 
                    } 
                    else if (mouse_x >= 26 && mouse_x <= 32) { //seek back 5 seconds
                        if (has_active_file && active_audio_file) { 
                            float pct = (float)(ui_current_sec - 5) / (float)ui_total_seconds; 
                            if (pct < 0.0f) pct = 0.0f; 
                            if (global_is_486) { 
                                long target_byte = (long)(global_wav_bytes * pct); int frame_size = target_channels * (active_bitdepth / 8); target_byte -= (target_byte % frame_size); fseek(active_audio_file, 44 + target_byte, SEEK_SET); 
                            } else { 
                                long track_len = file_is_native_wav ? wav_data_size : (file_size - start_off); 
                                long target_byte = (long)(track_len * pct); 
                                if (file_is_native_wav) { int bps = current_channels * (current_bitdepth / 8); target_byte -= (target_byte % bps); }
                                fseek(active_audio_file, start_off + target_byte, SEEK_SET); 
                                stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
                                current_ram_ptr = stream_buffer; 
                                if (!file_is_native_wav) mp3dec_init(&mp3d); 
                            } 
                            ui_total_samples_played = (unsigned long)((float)ui_total_seconds * target_sample_rate * pct); 
                            resample_pos = 0; pcm_leftover_bytes = 0; force_ui_redraw = 1; 
                        } 
                    } 
                    else if (mouse_x >= 34 && mouse_x <= 44) { //play/pause toggle
                        if (has_active_file) {
                            is_paused = !is_paused;
                        } 
                    } 
                    else if (mouse_x >= 46 && mouse_x <= 52) { //seek forward 5 seconds
                        if (has_active_file && active_audio_file) { 
                            float pct = (float)(ui_current_sec + 5) / (float)ui_total_seconds; 
                            if (pct > 1.0f) pct = 1.0f; 
                            if (global_is_486) { 
                                long target_byte = (long)(global_wav_bytes * pct); int frame_size = target_channels * (active_bitdepth / 8); target_byte -= (target_byte % frame_size); fseek(active_audio_file, 44 + target_byte, SEEK_SET); 
                            } else { 
                                long track_len = file_is_native_wav ? wav_data_size : (file_size - start_off); 
                                long target_byte = (long)(track_len * pct); 
                                if (file_is_native_wav) { int bps = current_channels * (current_bitdepth / 8); target_byte -= (target_byte % bps); }
                                fseek(active_audio_file, start_off + target_byte, SEEK_SET); 
                                stream_bytes = fread(stream_buffer, 1, 65536, active_audio_file); 
                                current_ram_ptr = stream_buffer; 
                                if (!file_is_native_wav) mp3dec_init(&mp3d); 
                            } 
                            ui_total_samples_played = (unsigned long)((float)ui_total_seconds * target_sample_rate * pct); 
                            resample_pos = 0; pcm_leftover_bytes = 0; force_ui_redraw = 1; 
                        } 
                    } 
                    else if (mouse_x >= 54 && mouse_x <= 58) { //loop toggle
                        if (has_active_file) { 
                            is_looping = !is_looping; 
                        } 
                    } 
                    else if (mouse_y == 22) { //volume control
                        if (mouse_x >= 67 && mouse_x <= 70) { 
                            set_volume(master_volume - 5); 
                        } 
                        else if (mouse_x >= 74 && mouse_x <= 77) { 
                            set_volume(master_volume + 5); 
                        } 
                    } 
                }
            } 
            else if (ui_view == 1) { 
                if (mouse_y == 4 && mouse_x >= 64 && mouse_x <= 75) { //search box click
                    active_input_field = 2; 
                    active_tab_element = 2; 
                    dialog_search_cursor = (int)strlen(dialog_search_text); 
                    browser_needs_redraw = 1; 
                } 
                else if (mouse_y == 4 && mouse_x >= 18 && mouse_x <= 58) { //address bar click
                    active_input_field = 3; 
                    active_tab_element = 1; 
                    dialog_address_cursor = (int)strlen(dialog_address_text); 
                    browser_needs_redraw = 1; 
                }
                else if (mouse_y == 20 && mouse_x >= 18 && mouse_x <= 57) { //filename box click
                    active_input_field = 4; 
                    active_tab_element = 4; 
                    dialog_filename_cursor = (int)strlen(dialog_file_name); 
                    browser_needs_redraw = 1; 
                }
                else { if (active_input_field != 0) browser_needs_redraw = 1; active_input_field = 0; }
                if (mouse_y >= 9 && mouse_y <= 17 && mouse_x >= 18 && mouse_x <= 74) { int clicked_idx = file_scroll + (mouse_y - 9); if (clicked_idx < file_count) { clock_t current_click_time = clock(); if (clicked_idx == last_clicked_file_idx && (current_click_time - last_file_click_time) < (CLOCKS_PER_SEC / 2)) { execute_browser_ok(); last_clicked_file_idx = -1; } else { file_selected = clicked_idx; active_tab_element = 3; strcpy(dialog_file_name, file_list[file_selected].name); dialog_filename_cursor = (int)strlen(dialog_file_name); browser_needs_redraw = 1; last_clicked_file_idx = clicked_idx; last_file_click_time = current_click_time; } } }
                else if (mouse_y == 20 && mouse_x >= 70 && mouse_x <= 77) { if (has_mouse) hide_mouse(); ui_view = 0; force_ui_redraw = 1; clear_inner_ui(); init_ui(app_filename); if (has_mouse) show_mouse(); }
                else if (mouse_y == 20 && mouse_x >= 61 && mouse_x <= 68) { execute_browser_ok(); }
                else if (mouse_y >= 9 && mouse_y <= 14 && mouse_x >= 2 && mouse_x <= 14) { char drive = 'C' + (mouse_y - 9); char temp_path[10]; sprintf(temp_path, "%c:\\", drive); if (access(temp_path, 0) == 0) { if (has_mouse) hide_mouse(); strcpy(current_dir, temp_path); strcpy(browser_status_msg, ""); load_dir(current_dir, 1); if (has_mouse) show_mouse(); } else { strcpy(browser_status_msg, "Directory doesnt exist"); browser_needs_redraw = 1; } }
                else if (mouse_y == 7 && mouse_x == 76) { if (file_scroll > 0) { file_scroll--; browser_needs_redraw = 1; } }
                else if (mouse_y == 17 && mouse_x == 76) { if (file_scroll < file_count - 9) { file_scroll++; browser_needs_redraw = 1; } }
                else if (mouse_y == 4 && mouse_x >= 2 && mouse_x <= 5) { if (path_history_index > 0) { path_history_index--; strcpy(browser_status_msg, ""); load_dir(path_history[path_history_index], 0); } }
                else if (mouse_y == 4 && mouse_x >= 7 && mouse_x <= 10) { if (path_history_index < path_history_count - 1) { path_history_index++; strcpy(browser_status_msg, ""); load_dir(path_history[path_history_index], 0); } }
                else if (mouse_y == 4 && mouse_x >= 12 && mouse_x <= 14) { char new_path[512]; int len = (int)strlen(current_dir); if (len > 3) { len -= 2; while (len > 0 && current_dir[len] != '\\' && current_dir[len] != '/') len--; strncpy(new_path, current_dir, len + 1); new_path[len + 1] = '\0'; strcpy(browser_status_msg, ""); load_dir(new_path, 1); } }
                else if (mouse_y == 1 && mouse_x >= 76 && mouse_x <= 78) { if (has_mouse) hide_mouse(); ui_view = 0; force_ui_redraw = 1; clear_inner_ui(); init_ui(app_filename); if (has_mouse) show_mouse(); }
            }
            else if (ui_view == 2) { //settings page
                if (mouse_y == 1 && mouse_x >= 75 && mouse_x <= 78) { //close settings button
                    keep_playing = 0;
                }
                else if (mouse_y == 3) { //tab bar
                    if (mouse_x >= 17 && mouse_x <= 23) { //audio settings
                        current_settings_tab = 3; 
                        browser_needs_redraw = 1; 
                    } 
                    else if (mouse_x >= 25 && mouse_x <= 33) { //display settings
                        current_settings_tab = 0; 
                        browser_needs_redraw = 1; 
                    } 
                    else if (mouse_x >= 35 && mouse_x <= 44) { //graphics settings
                        current_settings_tab = 1; 
                        browser_needs_redraw = 1; 
                    } 
                    else if (mouse_x >= 46 && mouse_x <= 52) { //other settings
                        current_settings_tab = 2; 
                        browser_needs_redraw = 1; 
                    } 
                    else if (mouse_x >= 54 && mouse_x <= 61) { //visual settings
                        current_settings_tab = 4; 
                        browser_needs_redraw = 1; 
                    } 
                    else if (mouse_x >= 71 && mouse_x <= 79) { //back button
                        if (has_mouse) hide_mouse(); 
                        ui_view = 0; 
                        force_ui_redraw = 1; 
                        init_ui(app_filename); 
                        if (has_mouse) show_mouse(); 
                    } 
                }
                else if (current_settings_tab == 1) {
                    if (mouse_y == 7 && mouse_x >= 4 && mouse_x <= 20) { setting_hover_effects = 0; browser_needs_redraw = 1; } 
                    else if (mouse_y == 8 && mouse_x >= 4 && mouse_x <= 20) { setting_hover_effects = 1; browser_needs_redraw = 1; } 
                    // --- WIRE UP THE MISSING ENABLE COLOR BUTTON! ---
                    else if (mouse_y == 11 && mouse_x >= 2 && mouse_x <= 20) { 
                        // Paralyze the button completely if MDA is detected!
                        if (video_address != 0xB0000) { 
                            setting_color = !setting_color; 
                            browser_needs_redraw = 1; 
                            if (has_mouse) hide_mouse(); force_ui_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse(); 
                        }
                    }
                    // ------------------------------------------------
                    else if (mouse_y == 14 && mouse_x >= 4 && mouse_x <= 30) { setting_ansi_mode = 0; browser_needs_redraw = 1; } 
                    else if (mouse_y == 15 && mouse_x >= 4 && mouse_x <= 30) { setting_ansi_mode = 1; browser_needs_redraw = 1; }
                    if (mouse_y >= 18 && mouse_y <= 21) { 
                        int col_idx = -1; if (mouse_x >= 27 && mouse_x <= 35) col_idx = 0; 
                        else if (mouse_x >= 38 && mouse_x <= 46) col_idx = 1; else if (mouse_x >= 49 && mouse_x <= 56) col_idx = 2; 
                        else if (mouse_x >= 59 && mouse_x <= 67) col_idx = 3; else if (mouse_x >= 70 && mouse_x <= 76) col_idx = 4; 
                        if (col_idx != -1) {
                            if (mouse_y == 18) { 
                                toggle_theme_color(&setting_bg_color, col_idx); 
                                enforce_theme_contrast(1); 
                            } 
                            else if (mouse_y == 19) { 
                                toggle_theme_color(&setting_fg_color, col_idx); 
                                enforce_theme_contrast(2); 
                            } 
                            else if (mouse_y == 20) { 
                                toggle_theme_color(&setting_sel_color, col_idx); 
                                enforce_theme_contrast(0); 
                            } 
                            else { 
                                toggle_theme_color(&setting_crit_color, col_idx); enforce_theme_contrast(0); 
                            } 
                            browser_needs_redraw = 1; 
                            if (mouse_y == 18 || mouse_y == 19) { 
                                if (has_mouse) hide_mouse(); force_ui_redraw = 1; init_ui(app_filename); if (has_mouse) show_mouse(); 
                            } 
                        } 
                    }
                }
                else if (current_settings_tab == 3) {
                    int setting_changed = 0;
                    if (mouse_y == 8) { 
                        if (mouse_x >= 26 && mouse_x <= 42 && config_is_pc_speaker != 0) { 
                            config_is_pc_speaker = 0; 
                            setting_changed = 1; 
                        } 
                        else if (mouse_x >= 51 && mouse_x <= 68 && config_is_pc_speaker != 1) { 
                            config_is_pc_speaker = 1; 
                            setting_changed = 1; 
                        } 
                    }
                    else if (mouse_y == 10) { if (mouse_x >= 26 && mouse_x <= 39 && custom_sample_rate != 0) { custom_sample_rate = 0; setting_changed = 1; } else if (mouse_x >= 42 && mouse_x <= 48 && custom_sample_rate != 22050) { custom_sample_rate = 22050; setting_changed = 1; } else if (mouse_x >= 51 && mouse_x <= 57 && custom_sample_rate != 11025) { custom_sample_rate = 11025; setting_changed = 1; } else if (mouse_x >= 61 && mouse_x <= 66 && custom_sample_rate != 8000) { custom_sample_rate = 8000; setting_changed = 1; } }
                    else if (mouse_y == 12) { if (mouse_x >= 26 && mouse_x <= 40 && custom_channels != 2) { custom_channels = 2; setting_changed = 1; } else if (mouse_x >= 51 && mouse_x <= 65 && custom_channels != 1) { custom_channels = 1; setting_changed = 1; } }
                    else if (mouse_y == 14) { if (mouse_x >= 26 && mouse_x <= 38 && custom_buffer_size != 16384) { custom_buffer_size = 16384; setting_changed = 1; } else if (mouse_x >= 42 && mouse_x <= 49 && custom_buffer_size != 32768) { custom_buffer_size = 32768; setting_changed = 1; } else if (mouse_x >= 51 && mouse_x <= 65 && custom_buffer_size != 65536) { custom_buffer_size = 65536; setting_changed = 1; } }
                    else if (mouse_y == 16) { if (mouse_x >= 26 && mouse_x <= 31 && pc_speaker_overdrive != 100) { pc_speaker_overdrive = 100; setting_changed = 1; } else if (mouse_x >= 33 && mouse_x <= 40 && pc_speaker_overdrive != 150) { pc_speaker_overdrive = 150; setting_changed = 1; } else if (mouse_x >= 42 && mouse_x <= 47 && pc_speaker_overdrive != 200) { pc_speaker_overdrive = 200; setting_changed = 1; } else if (mouse_x >= 51 && mouse_x <= 56 && pc_speaker_overdrive != 300) { pc_speaker_overdrive = 300; setting_changed = 1; } }
                    else if (mouse_y == 18) { //486 mode settings
                        if (mouse_x >= 26 && mouse_x <= 36 && !config_is_486) { 
                            config_is_486 = 1; 
                            setting_changed = 1; 
                        } 
                        else if (mouse_x >= 51 && mouse_x <= 62 && config_is_486) { 
                            config_is_486 = 0; setting_changed = 1; 
                        } 
                    }
                    else if (mouse_y == 22 && mouse_x >= 63 && mouse_x <= 76) { save_config(); settings_saved_flag = 1; browser_needs_redraw = 1; }
                    if (setting_changed) { browser_needs_redraw = 1; }
                }
                else if (current_settings_tab == 4) {
                    int setting_changed = 0;
                    if (mouse_y == 7) {
                        if (mouse_x >= 4 && mouse_x <= 13) { vis_vu_peaks = !vis_vu_peaks; setting_changed = 1; }
                        else if (mouse_x >= 32 && mouse_x <= 39) { vis_vu_falloff = 0; setting_changed = 1; }
                        else if (mouse_x >= 41 && mouse_x <= 50) { vis_vu_falloff = 1; setting_changed = 1; }
                        else if (mouse_x >= 52 && mouse_x <= 59) { vis_vu_falloff = 2; setting_changed = 1; }
                    }
                    else if (mouse_y == 8) {
                        // Cycles the DOS colors strictly from 8 (Dark Gray) to 15 (Bright White)
                        if (mouse_x >= 17 && mouse_x <= 19) { vis_vu_c1--; if (vis_vu_c1 < 8) vis_vu_c1 = 15; setting_changed = 1; }
                        else if (mouse_x >= 23 && mouse_x <= 25) { vis_vu_c1++; if (vis_vu_c1 > 15) vis_vu_c1 = 8; setting_changed = 1; }
                        else if (mouse_x >= 40 && mouse_x <= 42) { vis_vu_c2--; if (vis_vu_c2 < 8) vis_vu_c2 = 15; setting_changed = 1; }
                        else if (mouse_x >= 46 && mouse_x <= 48) { vis_vu_c2++; if (vis_vu_c2 > 15) vis_vu_c2 = 8; setting_changed = 1; }
                        else if (mouse_x >= 63 && mouse_x <= 65) { vis_vu_c3--; if (vis_vu_c3 < 8) vis_vu_c3 = 15; setting_changed = 1; }
                        else if (mouse_x >= 69 && mouse_x <= 71) { vis_vu_c3++; if (vis_vu_c3 > 15) vis_vu_c3 = 8; setting_changed = 1; }
                    }
                    else if (mouse_y == 11) {
                        // Cycles the DOS colors strictly from 8 (Dark Gray) to 15 (Bright White)
                        if (mouse_x >= 4 && mouse_x <= 13) { vis_bar_peaks = !vis_bar_peaks; setting_changed = 1; }
                        else if (mouse_x >= 32 && mouse_x <= 39) { vis_bar_falloff = 0; setting_changed = 1; }
                        else if (mouse_x >= 41 && mouse_x <= 50) { vis_bar_falloff = 1; setting_changed = 1; }
                        else if (mouse_x >= 52 && mouse_x <= 59) { vis_bar_falloff = 2; setting_changed = 1; }
                    }
                    else if (mouse_y == 12) {
                        // Cycles the DOS colors strictly from 8 (Dark Gray) to 15 (Bright White)
                        if (mouse_x >= 17 && mouse_x <= 19) { vis_bar_c1--; if (vis_bar_c1 < 8) vis_bar_c1 = 15; setting_changed = 1; }
                        else if (mouse_x >= 23 && mouse_x <= 25) { vis_bar_c1++; if (vis_bar_c1 > 15) vis_bar_c1 = 8; setting_changed = 1; }
                        else if (mouse_x >= 40 && mouse_x <= 42) { vis_bar_c2--; if (vis_bar_c2 < 8) vis_bar_c2 = 15; setting_changed = 1; }
                        else if (mouse_x >= 46 && mouse_x <= 48) { vis_bar_c2++; if (vis_bar_c2 > 15) vis_bar_c2 = 8; setting_changed = 1; }
                        else if (mouse_x >= 63 && mouse_x <= 65) { vis_bar_c3--; if (vis_bar_c3 < 8) vis_bar_c3 = 15; setting_changed = 1; }
                        else if (mouse_x >= 69 && mouse_x <= 71) { vis_bar_c3++; if (vis_bar_c3 > 15) vis_bar_c3 = 8; setting_changed = 1; }
                    }
                    else if (mouse_y == 13) {
                        if (mouse_x >= 17 && mouse_x <= 25) { vis_bar_style = 0; setting_changed = 1; }
                        else if (mouse_x >= 27 && mouse_x <= 36) { vis_bar_style = 1; setting_changed = 1; }
                    }
                    else if (mouse_y == 16) {
                        if (mouse_x >= 17 && mouse_x <= 24) { vis_dbg_refresh = 0; setting_changed = 1; }
                        else if (mouse_x >= 26 && mouse_x <= 34) { vis_dbg_refresh = 1; setting_changed = 1; }
                        else if (mouse_x >= 36 && mouse_x <= 45) { vis_dbg_refresh = 2; setting_changed = 1; }
                        else if (mouse_x >= 61 && mouse_x <= 67) { vis_dbg_detail = 0; setting_changed = 1; }
                        else if (mouse_x >= 69 && mouse_x <= 75) { vis_dbg_detail = 1; setting_changed = 1; }
                    }
                    else if (mouse_y == 18) {
                        if (mouse_x >= 18 && mouse_x <= 29) { vis_fps_cap = 0; setting_changed = 1; }
                        else if (mouse_x >= 31 && mouse_x <= 40) { vis_fps_cap = 1; setting_changed = 1; }
                        else if (mouse_x >= 42 && mouse_x <= 51) { vis_fps_cap = 2; setting_changed = 1; }
                    }
                    else if (mouse_y == 22 && mouse_x >= 63 && mouse_x <= 76) { 
                        save_config(); settings_saved_flag = 1; browser_needs_redraw = 1; 
                    }
                    if (setting_changed) { browser_needs_redraw = 1; }
                }
            }
            else if (ui_view == 3) {//playlist view
                if (mouse_y == 1) { //top bar clicks
                    if (mouse_x >= 1 && mouse_x <= 6) active_menu = 1; 
                    else if (mouse_x >= 8 && mouse_x <= 14) active_menu = 2; 
                    else if (mouse_x >= 16 && mouse_x <= 23) active_menu = 3; 
                    else if (mouse_x >= 75 && mouse_x <= 78) keep_playing = 0; 
                }
                else if (mouse_y == 22 && mouse_x >= 2 && mouse_x <= 6) { //back to main screen button
                    if (has_mouse) hide_mouse(); 
                    ui_view = 0; 
                    browser_needs_redraw = 1; 
                    force_ui_redraw = 1; 
                    init_ui(app_filename); 
                    if (has_mouse) show_mouse(); 
                }
                else if (mouse_y >= 5 && mouse_y <= 15 && mouse_x >= 1 && mouse_x <= 76) { //playlist item clicks
                    int clicked_idx = queue_scroll + (mouse_y - 5); 
                    if (clicked_idx < queue_count) { 
                        clock_t current_click_time = clock(); 
                        if (clicked_idx == last_clicked_file_idx && (current_click_time - last_file_click_time) < (CLOCKS_PER_SEC / 2)) { 
                            load_new_file_from_browser(queue_paths[clicked_idx]); 
                            last_clicked_file_idx = -1; 
                        } 
                        else { 
                            queue_selected = clicked_idx; 
                            browser_needs_redraw = 1; last_clicked_file_idx = clicked_idx; 
                            last_file_click_time = current_click_time; 
                        } 
                    } 
                }
                else if (mouse_y == 19 && mouse_x >= 9 && mouse_x <= 70) { 
                    if (has_active_file && ui_total_seconds > 0) { 
                        float pct = (float)(mouse_x - 9) / 61.0f; 
                        if (pct < 0.0f) pct = 0.0f; if (pct > 1.0f) pct = 1.0f; 
                        if (global_is_486) { long target_byte = (long)(global_wav_bytes * pct); 
                            int frame_size = target_channels * (active_bitdepth / 8); target_byte -= (target_byte % frame_size); 
                            current_ram_ptr = pcm_ram_cache + target_byte; pcm_bytes_remaining = global_wav_bytes - target_byte; 
                        } 
                        else { 
                            long track_len = file_size - start_off; 
                            long target_byte = (long)(track_len * pct); 
                            current_ram_ptr = mp3_ram_cache + start_off + target_byte; 
                            bytes_remaining = track_len - target_byte; resample_pos = 0; 
                            pcm_leftover_bytes = 0; mp3dec_init(&mp3d); 
                        } 
                        ui_total_samples_played = (unsigned long)((float)ui_total_seconds * target_sample_rate * pct); 
                        force_ui_redraw = 1; 
                    } 
                }
                else if (mouse_y == 22) { //bottom button clicks
                    if (mouse_x >= 8 && mouse_x <= 12) { //settings button
                        if (has_mouse) hide_mouse(); 
                        ui_view = 2; 
                        browser_needs_redraw = 1; 
                        force_ui_redraw = 1; 
                        if (has_mouse) show_mouse(); 
                    } 
                    else if (mouse_x >= 20 && mouse_x <= 24) { //stop button
                        if (has_active_file) { 
                            if (global_is_486) { 
                                current_ram_ptr = pcm_ram_cache; 
                                pcm_bytes_remaining = global_wav_bytes; 
                            } 
                            else { 
                                current_ram_ptr = mp3_ram_cache + start_off; 
                                bytes_remaining = file_size - start_off; 
                                resample_pos = 0; pcm_leftover_bytes = 0; 
                            } 
                            ui_total_samples_played = 0; is_paused = 1; 
                        } 
                    } 
                    else if (mouse_x >= 26 && mouse_x <= 32) { //seek back 5 seconds
                        if (has_active_file) { 
                            int samples_to_skip = target_sample_rate * 5; 
                            if (global_is_486) { 
                                int bytes_to_skip = samples_to_skip * target_channels * (active_bitdepth / 8); 
                                if (current_ram_ptr - pcm_ram_cache >= bytes_to_skip) { 
                                    pcm_bytes_remaining += bytes_to_skip; 
                                    current_ram_ptr -= bytes_to_skip; 
                                    ui_total_samples_played -= samples_to_skip; 
                                } else { 
                                    current_ram_ptr = pcm_ram_cache; 
                                    pcm_bytes_remaining = global_wav_bytes; 
                                    ui_total_samples_played = 0; 
                                } 
                            } 
                            else { 
                                int bytes_to_skip = 16000 * 5; 
                                if (current_ram_ptr - (mp3_ram_cache + start_off) >= bytes_to_skip) { 
                                    bytes_remaining += bytes_to_skip; current_ram_ptr -= bytes_to_skip; 
                                    if (ui_total_samples_played > samples_to_skip) ui_total_samples_played -= samples_to_skip; 
                                    else ui_total_samples_played = 0; pcm_leftover_bytes = 0; 
                                } 
                                else { 
                                    current_ram_ptr = mp3_ram_cache + start_off; 
                                    bytes_remaining = file_size - start_off; 
                                    ui_total_samples_played = 0; 
                                    pcm_leftover_bytes = 0; 
                                } 
                            } 
                        } 
                    } 
                    else if (mouse_x >= 34 && mouse_x <= 44) { //play/pause toggle
                        if (has_active_file) { 
                            is_paused = !is_paused; 
                        } 
                    } 
                    else if (mouse_x >= 46 && mouse_x <= 52) { //seek forward 5 seconds
                        if (has_active_file) { 
                            int samples_to_skip = target_sample_rate * 5; 
                            if (global_is_486) { 
                                int bytes_to_skip = samples_to_skip * target_channels * (active_bitdepth / 8); 
                                if (pcm_bytes_remaining > bytes_to_skip) { pcm_bytes_remaining -= bytes_to_skip; 
                                    current_ram_ptr += bytes_to_skip; ui_total_samples_played += samples_to_skip; 
                                } 
                            } 
                            else { 
                                int bytes_to_skip = 16000 * 5; 
                                if (bytes_remaining > bytes_to_skip) { 
                                    bytes_remaining -= bytes_to_skip; 
                                    current_ram_ptr += bytes_to_skip; 
                                    ui_total_samples_played += samples_to_skip; 
                                    pcm_leftover_bytes = 0; 
                                } 
                            } 
                        } 
                    } 
                    else if (mouse_x >= 54 && mouse_x <= 58) { 
                        if (has_active_file) { 
                            is_looping = !is_looping; 
                        } 
                    } 
                    else if (mouse_x >= 67 && mouse_x <= 70) { 
                        set_volume(master_volume - 5); 
                    } 
                    else if (mouse_x >= 74 && mouse_x <= 77) { 
                        set_volume(master_volume + 5); 
                    } 
                }
                else if (mouse_y == 17) {
                    if (mouse_x >= 1 && mouse_x <= 19) { 
                        if (has_mouse) hide_mouse(); 
                        ui_view = 1; 
                        browser_enqueue_only = 0; 
                        browser_save_mode = 1; 
                        getcwd(current_dir, 512); 
                        load_dir(current_dir, 1); 
                        active_tab_element = 4; 
                        active_input_field = 4; 
                        strcpy(dialog_file_name, "playlist.m3u"); 
                        dialog_filename_cursor = strlen(dialog_file_name); 
                        init_ui(app_filename); 
                        if (has_mouse) show_mouse(); 
                    } 
                    else if (mouse_x >= 21 && mouse_x <= 34) { 
                        if (has_mouse) hide_mouse(); 
                        ui_view = 1; active_tab_element = 0; 
                        active_input_field = 0; 
                        browser_enqueue_only = 1; 
                        getcwd(current_dir, 512); 
                        load_dir(current_dir, 1); 
                        init_ui(app_filename); 
                        if (has_mouse) show_mouse(); 
                    } 
                    else if (mouse_x >= 36 && mouse_x <= 48) { 
                        queue_count = 0; 
                        queue_current = -1; 
                        queue_selected = -1; 
                        queue_scroll = 0; 
                        advance_playlist = 1; 
                        browser_needs_redraw = 1; 
                    } 
                    else if (mouse_x >= 50 && mouse_x <= 60) { 
                        if (queue_selected >= 0 && queue_selected < queue_count - 1) { 
                            char temp_path[512], temp_disp[64]; 
                            strcpy(temp_path, queue_paths[queue_selected+1]); 
                            strcpy(temp_disp, queue_displays[queue_selected+1]); 
                            strcpy(queue_paths[queue_selected+1], queue_paths[queue_selected]); 
                            strcpy(queue_displays[queue_selected+1], queue_displays[queue_selected]); 
                            strcpy(queue_paths[queue_selected], temp_path); 
                            strcpy(queue_displays[queue_selected], temp_disp); 
                            if (queue_current == queue_selected) queue_current++; 
                            else if (queue_current == queue_selected + 1) queue_current--; queue_selected++; 
                            browser_needs_redraw = 1; 
                        } 
                    } 
                    else if (mouse_x >= 62 && mouse_x <= 70) { 
                        if (queue_selected > 0 && queue_selected < queue_count) { 
                            char temp_path[512], temp_disp[64]; 
                            strcpy(temp_path, queue_paths[queue_selected-1]); 
                            strcpy(temp_disp, queue_displays[queue_selected-1]); 
                            strcpy(queue_paths[queue_selected-1], queue_paths[queue_selected]); 
                            strcpy(queue_displays[queue_selected-1], queue_displays[queue_selected]); 
                            strcpy(queue_paths[queue_selected], temp_path); strcpy(queue_displays[queue_selected], temp_disp); 
                            if (queue_current == queue_selected) queue_current--; 
                            else if (queue_current == queue_selected - 1) queue_current++; 
                            queue_selected--; browser_needs_redraw = 1; 
                        } 
                    } 
                    else if (mouse_x >= 72 && mouse_x <= 76) { 
                        if (queue_selected >= 0 && queue_selected < queue_count) { 
                            for (int i = queue_selected; i < queue_count - 1; i++) { 
                                strcpy(queue_paths[i], queue_paths[i+1]); 
                                strcpy(queue_displays[i], queue_displays[i+1]); 
                            } 
                            queue_count--; 
                            if (queue_current == queue_selected) { 
                                advance_playlist = 1; queue_current = -1; 
                            } 
                            else if (queue_current > queue_selected) { 
                                queue_current--; 
                            } 
                            if (queue_selected >= queue_count) queue_selected = queue_count - 1; 
                            if (queue_scroll > 0 && queue_scroll > queue_count - 11) queue_scroll--; 
                            browser_needs_redraw = 1; 
                        } 
                    } 
                    else if (mouse_x == 78) { 
                        if (queue_scroll < queue_count - 11) { 
                            queue_scroll++; browser_needs_redraw = 1; 
                        } 
                    } 
                }
                else if (mouse_y == 3 && mouse_x == 78) { 
                    if (queue_scroll > 0) { queue_scroll--; 
                        browser_needs_redraw = 1; 
                    } 
                }
            }
        }
        if (mouse_right && !prev_mouse_right) { //right click to deselect
            if (ui_view == 3 && queue_selected != -1) { 
                queue_selected = -1; 
                browser_needs_redraw = 1; 
            }
            else if (ui_view == 1 && file_selected != -1) { 
                file_selected = -1; 
                browser_needs_redraw = 1; 
            }
        }
        prev_mouse_left = mouse_left; 
        prev_mouse_right = mouse_right;
    }
    
    close_audio_engine(); //close the audio engine before we do the final screen cleanup and exit, to minimize the chances of a crash on exit due to the audio engine trying to access freed memory after we've cleaned up the screen and exited the main loop
    save_config(); //save config on exit just in case, to preserve any settings changes the user made during the session that they didnt manually save in the settings menu
    if (has_mouse) hide_mouse(); //hide the mouse before we do the final screen cleanup, to prevent any chance of the mouse cursor being left visible on the screen after the program exits
    for(int r = 0; r < 25; r++) { //clearing screen using for loops instead of system("cls") to avoid the overhead and potential security issues of spawning a separate process just to clear the screen, and to ensure compatibility with all DOS environments including those where system() might not be available or might not work as expected
        for(int c = 0; c < 80; c++) set_char(r, c, ' ', 0x07); //set every character on the screen to a space with default color to effectively clear the screen
    }
    if (has_mouse) show_mouse(); hide_mouse(); //disable mouse pointer
    gotoxy(1, 1); _setcursortype(_NORMALCURSOR);  //restore blinking keyboard cursor
    //printf("Playback finished.\n");
    return 0; //program exits without any error
}