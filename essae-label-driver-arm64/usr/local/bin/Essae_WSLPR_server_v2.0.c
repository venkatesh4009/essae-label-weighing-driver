#define _XOPEN_SOURCE 700  // Must be first!
#define _DEFAULT_SOURCE

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <json-c/json.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <errno.h>
#include <stddef.h>

// ------ Networking headers ---------------------------------------------------------

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>

#define PORT 8888
#define BUFFER_SIZE 2048
#define MAX_PATH 512
#define INITIAL_CAP 16384

#define LFT_DB_PATH "SQL_LFT_Files.db"
#define DRIVER_VERSION "v2.0.0"

#define ESC 0x1B
#define GS  0x1D
#define FS  0x1C
#define LF  0x0A

// ----- Printer-specific Units ------------------------------------------------------

#define DOTS_PER_MM     8.0f           // 1 mm = 8 dots
#define DOTSIZE         0.125f         // 1 dot = 0.125 mm
#define MAX_DOTS        432            // 432 dot head
#define MAX_MM          (MAX_DOTS / DOTS_PER_MM)  // 54.0 mm max width
#define DEFAULT_LINE_SPACING_MM 3.0f
#define MAX_BITMAP_SIZE 4096

#define MAX_ING_LINES 10
#define MAX_ING_LINE_LEN 128

#define WEIGH 1
#define PCS   0

int lbl_wtgrams = 1;
int uom_type = 0;

double unit_price = 0.0;          // 5 – Unit Price
double actual_unit_price = 0.0;   // 73 – Actual Unit Price
char uom[32] = "";
char guom[32] = "";
char spl_up[32] = "";
char raw[4096];           // Temporary buffer for reading image or skipping line

// ------- Globals ---------------------------------------------------------------------

static float lbl_width_mm;    // full label width in mm (from ~S)
static float lbl_height_mm;   // full label height in mm (from ~S)
static float lbl_x_offset = 0.0f;      // tune this so x=0 lines up
static float lbl_y_offset = 0.0f;      // tune this so y=0 lines up
float last_text_y = 0.0f;
static int gui_data_id = 0;
static int num_json_barcodes = 0;

extern double actual_unit_price;
extern double unit_price;
extern int uom_type;

// ----- Helpers -------------------------------------------------------------------------

static inline uint8_t lo(int v) { return v & 0xFF; }
static inline uint8_t hi(int v) { return (v >> 8) & 0xFF; }

// ---- Store label dimensions (from ~S) so send_text() can use them: --------------------

static float label_width_mm;
static float label_height_mm;

static struct json_object *json_root = NULL;

int weight_fd;
pthread_mutex_t weight_mutex = PTHREAD_MUTEX_INITIALIZER;

// ------- Forward_declarations ----------------------------------------------------------

int setup_server_socket(int port);
void handle_client(int client_fd);
ssize_t write_all(int fd, const void *buf, size_t len);
void process_weight_line(int client_fd, const char *cmd);
int convert_label(const char *config_path, const char *lft_path);
ssize_t read_line(int fd, char *buf, size_t max);
char *trim_whitespace(char *str);

void set_printer_rotation(int fd, int angle);
void select_font(int fd, int font);
void set_text_size(int fd, float h, float w);
void set_absolute_position(int prn, int x_dots, int y_dots);

int LoadDBBarcodeRecord(int bcnum,
    char *out_data, char *out_type, char *out_name,
    char *out_fld1, char *out_cond1, char *out_shift1,
    char *out_fld2, char *out_cond2, char *out_shift2);
    
int GetBarcodeData(char *out_pattern, const char *barcode_type);

unsigned char CheckPrintStatus(char prnstatus);

// ------ Check_Print_Status --------------------------------------------------------------

unsigned char CheckPrintStatus(char prnstatus) {
    if (prnstatus == '0') return 0;  // Never print (No)
    if (prnstatus == '1') return 1;  // Always print (All)

    if (prnstatus == '2') return (uom_type == WEIGH); // Print Only Weighing (WEIGH)
    if (prnstatus == '3') return (uom_type == PCS);   // Print only Non Weighing (NON WEIGH Ex: PCS)

    if (prnstatus == '4') 				// WEIGH & Special Price
        return (uom_type == WEIGH && fabs(unit_price - actual_unit_price) < 0.001);
    
    if (prnstatus == '5') 				// PCS & Special Price
        return (uom_type == PCS && fabs(unit_price - actual_unit_price) < 0.001);

    return 1; // Default: print
}

// ----- try_read -------------------------------------------------------------------------

static int try_read(int prn, char *buf, int buflen) {
    int n = read(prn, buf, buflen - 1);
    if (n > 0) {
        buf[n] = '\0';
        return n;
    }
    return 0;
}


// ------ trim_whitespace -------------------------------------------------------------------

char *trim_whitespace(char *str) {
    if (!str) return NULL;

    // Trim leading spaces/tabs/newlines
    while (isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        // All spaces
        return str;
    }

    // Trim trailing spaces/tabs/newlines
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

// ------ Helper to write everything (handles short writes) ---------------------------------------

ssize_t write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = buf;
    while (total < len) {
        ssize_t n = write(fd, ptr + total, len - total);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

// ----- Read a line (up to '\n') from socket -------------------------------------------------------
 
ssize_t read_line(int fd, char *buf, size_t maxlen) {
    size_t i = 0;
    while (i + 1 < maxlen) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

// ------- Create, bind, and listen on TCP socket -----------------------------------------------------

int setup_server_socket(int port) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        exit(1);
    }
    int opt = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sfd);
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sfd);
        exit(1);
    }
    if (listen(sfd, 5) < 0) {
        perror("listen");
        close(sfd);
        exit(1);
    }
    return sfd;
}

static void *client_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    handle_client(client_fd);

    return NULL;
}

// ------- handle_client() that also handles printer mode -------------------------------------------------------

void handle_client(int client_fd) {
    char buf[BUFFER_SIZE];
    ssize_t cnt;

    while ((cnt = recv(client_fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[cnt] = '\0';
        pthread_mutex_lock(&weight_mutex);

        char *saveptr = NULL;
        char *line = strtok_r(buf, "\n", &saveptr);
        while (line) {
            char *cmd = trim_whitespace(line);
            if (cmd[0] == '\0') {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }

            if (strcmp(cmd, "MODE:PRINTER") == 0) {
                char *json_path = strtok_r(NULL, "\n", &saveptr);
                char *slot_str  = strtok_r(NULL, "\n", &saveptr);
                char *sel_id    = strtok_r(NULL, "\n", &saveptr);

                if (json_path && slot_str && sel_id) {
                    json_path = trim_whitespace(json_path);
                    slot_str  = trim_whitespace(slot_str);
                    gui_data_id = atoi(trim_whitespace(sel_id));

                    int rc = convert_label(json_path, slot_str);
                    write_all(client_fd, rc == 0 ? "OK\n" : "Error printing\n",
                              rc == 0 ? 3 : 15);
                } else {
                    write_all(client_fd, "Error: printer args missing\n", 28);
                }
                break;  // End of MODE:PRINTER block
            } else {
                process_weight_line(client_fd, cmd);
            }

            line = strtok_r(NULL, "\n", &saveptr);
        }

        pthread_mutex_unlock(&weight_mutex);
    }

    close(client_fd);
}



// ------- weighing_scle_commands :~ including MODE header -------------------------------------------

void process_weight_line(int client_fd, const char *cmd) {
    char response[BUFFER_SIZE] = {0};
    unsigned char c;

    // 1) MODE header: drop or ACK
    if (strcmp(cmd, "MODE:WEIGHT") == 0) {
        const char *ack = "OK:WEIGHT\n";
        write_all(client_fd, ack, strlen(ack));
        return;
    }

    // 2) Real scale commands
    if (strcmp(cmd, "RD_WEIGHT") == 0) {
        c = 0x05;
        write(weight_fd, &c, 1);
        usleep(200000);
        int r = read(weight_fd, response, sizeof(response) - 1);
        if (r <= 0) {
            strcpy(response, "Error: No response from weight machine.");
        }
    }
    else if (strcmp(cmd, "XC_TARE") == 0) {
        unsigned char tcmds[2] = {'T','t'};
        write(weight_fd, tcmds, 2);
        strcpy(response, "XC_TARE: Tare command sent.");
    }
    else if (strcmp(cmd, "XC_REZERO") == 0) {
        c = 0x10; write(weight_fd, &c, 1);
        strcpy(response, "XC_REZERO sent.");
    }
    else if (strcmp(cmd, "XC_SON") == 0) {
        c = 0x12; write(weight_fd, &c, 1);
        strcpy(response, "XC_SON: Calibration start.");
    }
    else if (strncmp(cmd, "XC_KEYCAL", 9) == 0) {
        c = 0x13; write(weight_fd, &c, 1);
        int payload_len = strlen(cmd) - 9;
        if (payload_len > 0) {
            write(weight_fd, cmd + 9, payload_len);
        }
        strcpy(response, "XC_KEYCAL sent with weight payload.");
    }
    else if (strcmp(cmd, "XC_CALZERO") == 0) {
        c = 0x14; write(weight_fd, &c, 1);
        strcpy(response, "XC_CALZERO: Zero point set.");
    }
    else if (strcmp(cmd, "XC_CALSPAN") == 0) {
        c = 0x15; write(weight_fd, &c, 1);
        strcpy(response, "XC_CALSPAN: Span set.");
    }
    else if (strcmp(cmd, "XC_CALIBRATE") == 0) {
        c = 0x16; write(weight_fd, &c, 1);
        strcpy(response, "XC_CALIBRATE: Calibration finalize.");
    }
    else if (strcmp(cmd, "XC_RDRAWCT") == 0) {
        c = 0x11; write(weight_fd, &c, 1);
        usleep(200000);
        int r = read(weight_fd, response, sizeof(response) - 1);
        if (r <= 0) {
            strcpy(response, "Error: No raw data response.");
        }
    }
    else if (strcmp(cmd, "XC_LOAD_DEFAULTS") == 0) {
        c = 0x17; write(weight_fd, &c, 1);
        strcpy(response, "XC_LOAD_DEFAULTS sent.");
    }
    else if (strcmp(cmd, "WR_TECHSPEC") == 0) {
    c = 0x18; 
    write(weight_fd, &c, 1);
    strcpy(response, "WR_TECHSPEC sent.");
  }
   else if (strcmp(cmd, "WR_CUSSPEC") == 0) {
    c = 0x1A; 
    write(weight_fd, &c, 1);
    strcpy(response, "WR_CUSSPEC sent.");
 }
   else if (strcmp(cmd, "RD_CUSSPEC") == 0) {
        unsigned char c = 0x1B;
        write(weight_fd, &c, 1);
        usleep(200000);

        int r = read(weight_fd, response, sizeof(response) - 1);
        if (r <= 0) {
            strcpy(response, "Error: no data from scale");
        } else {
            response[r] = '\0';
        }
    }
    else if (strcmp(cmd, "RD_TECHSPEC") == 0) {
        unsigned char c = 0x19;
        write(weight_fd, &c, 1);
        usleep(200000);

        // Read whatever ASCII the scale sends (e.g. "03 05 03 00 ...\r\n")
        int r = read(weight_fd, response, sizeof(response) - 1);
        if (r <= 0) {
            strcpy(response, "Error: no data from scale");
        } else {
            // Just null-terminate the ASCII string exactly as received:
            response[r] = '\0';
        }
    }

    else if (strcmp(cmd, "XC_RESTART") == 0) {
        c = 0x1C; write(weight_fd, &c, 1);
        strcpy(response, "XC_RESTART sent.");
    }
    else {
        strcpy(response, "Error: Unknown command");
    }

    // 3) Send response back
    write_all(client_fd, response, strlen(response));
}


// ------ Live data globals (with Data ID and Description) ---------------------------------------------------

// Group 1–15: PLU & Basic Info
static int    plu_id              = 0;   // 1  – PLU No (Unique product number)
static char   plu_name[64]        = ""; // 2  – PLU Name (Product name)
static char   plu_code[32]        = ""; // 3  – PLU Code (Internal product code)
// static char   guom[8]             = ""; // 4  – GUOM (Unit) (General Unit of Measure)
// static double unit_price          = 0.0; // 5  – Unit Price (Normal unit rate)
// static char   spl_up[32]          = ""; // 6  – Special Unit Price (Promotional price)
static int    quantity            = 0;   // 7  – Quantity (Number of units)
static double tare_wt             = 0.0; // 8  – Tare Weight (Packaging weight)
static double fixed_price         = 0.0; // 9  – Fixed Price (Any fixed price override)
static char   packed_date[16]     = ""; // 10 – Packed Date (Packaging date string)
static char   packed_time[16]     = ""; // 11 – Packed Time (Packaging time string)
static char   sellby_date[16]     = ""; // 12 – Sell By Date (Recommended sell-before date)
static char   sellby_time[16]     = ""; // 13 – Sell By Time (Recommended sell-before time)
static char   useby_date[16]      = ""; // 14 – Use By Date (Expiry date)
static char   useby_time[16]      = ""; // 15 – Use By Time (Expiry time)

// Group 16–40: Classification & Header/Footer
static double plu_minimum         = 0.0; // 16 – PLU Minimum (Minimum stock/weight)
static double plu_target          = 0.0; // 17 – PLU Target (Target stock/weight)
static double plu_maximum         = 0.0; // 18 – PLU Maximum (Maximum stock/weight)
static int    group_no            = 0;   // 19 – Group No (Product group ID)
static char   group_name[64]      = ""; // 20 – Group Name (Product group name)
static int    department_no       = 0;   // 21 – Department No (Department ID)
static char   department_name[64] = ""; // 22 – Department Name (Department label)
static int    tax_no              = 0;   // 23 – Tax No (Tax scheme ID)
static char   tax_name[64]        = ""; // 24 – Tax Name (Tax label, e.g. GST)
static char   tax_type[32]        = ""; // 25 – Tax Type (Inclusive/Exclusive)
static double tax_rate            = 0.0; // 26 – Tax Rate (Percentage)
static int    operator_no         = 0;   // 27 – Operator No (User/operator ID)
static char   operator_name[64]   = ""; // 28 – Operator Name (User/operator name)
static char   operator_password[32] = ""; // 29 – Operator Password (Not displayed)

static char   header1[64]         = ""; // 30 – Header1 (Custom header line 1)
static char   header2[64]         = ""; // 31 – Header2 (Custom header line 2)
static char   header3[64]         = ""; // 32 – Header3 (Custom header line 3)
static char   header4[64]         = ""; // 33 – Header4 (Custom header line 4)
static char   header5[64]         = ""; // 34 – Header5 (Custom header line 5)
static char   footer1[64]         = ""; // 35 – Footer1 (Custom footer line 1)
static char   footer2[64]         = ""; // 36 – Footer2 (Custom footer line 2)
static char   footer3[64]         = ""; // 37 – Footer3 (Custom footer line 3)
static char   footer4[64]         = ""; // 38 – Footer4 (Custom footer line 4)
static char   footer5[64]         = ""; // 39 – Footer5 (Custom footer line 5)
static int    discount_no         = 0;   // 40 – Discount No (Applied discount ID)

// Group 41–60: Promotion, Packaging, Barcode, Discount Info
static char   discount_name[64]   = ""; // 41 – Discount Name (Discount description)
static char   discount_type[16]   = ""; // 42 – Discount Type (Flat or Percentage)
static double discount_first_target  = 0.0; // 43 – Discount First Target (Threshold)
static double discount_first_value   = 0.0; // 44 – Discount First Value (Amount)
static double discount_second_target = 0.0; // 45 – Discount Second Target (Threshold)
static double discount_second_value  = 0.0; // 46 – Discount Second Value (Amount)
static char   discount_days[32]   = ""; // 47 – Discount Days (Applicable days)
static char   discount_start[32]  = ""; // 48 – Discount Start (Begin time/date)
static char   discount_end[32]    = ""; // 49 – Discount End (End time/date)
static char   package_type[32]    = ""; // 50 – Package Type (Packaging style)
static char   tare_name[64]       = ""; // 51 – Tare Name (Container name)
static double tare_value          = 0.0; // 52 – Tare Value (Container weight)
static char   storage_temp[32]    = ""; // 53 – Storage Temperature (Recommended storage)
static char   barcode_name[64]    = ""; // 54 – Barcode Name (Label name)
static char   barcode_type[32]    = ""; // 55 – Barcode Type (EAN13, CODE128, etc.)
static char   barcode_data[64]    = ""; // 56 – Barcode Data (Encoded string)
static char   bc_field1[32]       = ""; // 57 – BC Field1 (Barcode field 1 value)
static char   bc_field1_con[32]   = ""; // 58 – BC Field1 Concat (Concatenation rule)
static char   bc_field1_shift[32] = ""; // 59 – BC Field1 Shift (Shift rule)
static char   bc_field2[32]       = ""; // 60 – BC Field2 (Barcode field 2 value)

// Group 61–80: Ingredients, Label, Image, Billing
static char   bc_field2_con[32]   = ""; // 61 – BC Field2 Condition (Condition rule)
static char   bc_field2_shift[32] = ""; // 62 – BC Field2 Shift (Shift rule)
static int    ingredient_no       = 0;   // 63 – Ingredient No (Ingredient ID)
static char   ingredient_name[64] = ""; // 64 – Ingredient Name (Description)
static char   ingredients_text[512] = ""; // 65 – Ingredients Text (List)
static int    message_no          = 0;   // 66 – Message No (Message ID)
static char   message_name[64]    = ""; // 67 – Message Name (Title)
static char   message_text[512]   = ""; // 68 – Message Text (Content)
static double current_net_weight  = 0.0; // 69 – Current Net Weight (Net weight)
static double current_tare_weight = 0.0; // 70 – Current Tare Weight (Tare weight)
static double current_gross_weight= 0.0; // 71 – Current Gross Weight (Gross weight)
static double weight_or_quantity  = 0.0; // 72 – Weight or Quantity (Auto choose)
// static double actual_unit_price   = 0.0; // 73 – Actual Unit Price (Final rate)
static int    image_no            = 0;   // 74 – Image No (Image reference)
static char   image_file_name[32] = ""; // 75 – Image File Name (Filename)
static char   label_date_time[32] = ""; // 76 – Label DateTime (Timestamp)
static int    label_design_no     = 0;   // 77 – Label Design No (Template ID)
static char   label_file_name[32] = ""; // 78 – Label File Name (Filename)
static int    bill_no              = 0;   // 79 – Bill No (Transaction ID)
static char   scale_no[32]         = ""; // 80 – Scale No (Scale ID)

// Group 81–96: Final Totals, Output Info
static char   scale_name[64]      = ""; // 81 – Scale Name (Model name)
static double scale_capacity      = 0.0; // 82 – Scale Capacity (Max weight)
static double scale_accuracy      = 0.0; // 83 – Scale Accuracy (Precision)
static char   current_datetime[32]= ""; // 84 – Current DateTime (Timestamp)
static int    no_of_items         = 0;   // 85 – No of Items (Item count)
static double total_amount        = 0.0; // 86 – Total Amount (Sum amount)
static double total_quantity      = 0.0; // 87 – Total Quantity (Sum units)
static double total_weight        = 0.0; // 88 – Total Weight (Sum weight)
static double total_qty_or_weight = 0.0; // 89 – Total Qty/Weight (Best fit)
static double total_tax           = 0.0; // 90 – Total Tax (Tax amount)
static double total_discount      = 0.0; // 91 – Total Discount (Discount amount)
static int    today_bill_no        = 0;   // 92 – Today Bill No (Daily bill count)
static double total_price         = 0.0; // 93 – Total Price (Net price)
// static char   uom[8]              = ""; // 94 – Unit of Measure (e.g. KG, PCS)
static char   barcode_flag[32]    = ""; // 95 – Barcode Flag (Encoded flag)
static char   bill_text[128]      = ""; // 96 – Bill Text (Payment note)


// ----- LoadDBBarcodeRecord: Fetch from barcode_templates table ------------------------------------------

int LoadDBBarcodeRecord(int barcode_number,
    char *data, char *type, char *name,
    char *fld1, char *cond1, char *shift1,
    char *fld2, char *cond2, char *shift2)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;

    if (sqlite3_open(LFT_DB_PATH, &db) != SQLITE_OK)
        return 1;

    const char *sql =
        "SELECT barcode_data, barcode_type, barcode_name, "
        "barcode_fld1, fld1_condition, fld1_shift, "
        "barcode_fld2, fld2_condition, fld2_shift "
        "FROM barcode_templates WHERE barcode_number = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 1;
    }

    sqlite3_bind_int(stmt, 1, barcode_number);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        snprintf(data,   128, "%s", sqlite3_column_text(stmt, 0));
        snprintf(type,    16, "%s", sqlite3_column_text(stmt, 1));
        snprintf(name,    16, "%s", sqlite3_column_text(stmt, 2));
        snprintf(fld1,    16, "%s", sqlite3_column_text(stmt, 3));
        snprintf(cond1,    8, "%s", sqlite3_column_text(stmt, 4));
        snprintf(shift1,   4, "%s", sqlite3_column_text(stmt, 5));
        snprintf(fld2,    16, "%s", sqlite3_column_text(stmt, 6));
        snprintf(cond2,    8, "%s", sqlite3_column_text(stmt, 7));
        snprintf(shift2,   4, "%s", sqlite3_column_text(stmt, 8));
    } else {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 2;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}


// ------ load_json_data ------------------------------------------------------------------------------

void load_json_data(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen"); exit(EXIT_FAILURE); }

    struct stat st;
    if (fstat(fileno(f), &st) != 0) { perror("fstat"); fclose(f); exit(EXIT_FAILURE); }

    char *data = malloc(st.st_size + 1);
    if (!data) { perror("malloc"); fclose(f); exit(EXIT_FAILURE); }

    fread(data, 1, st.st_size, f);
    data[st.st_size] = '\0';
    fclose(f);

    // Parse JSON once
    json_root = json_tokener_parse(data);
       if (!json_root) {
        fprintf(stderr, "JSON parse error\n");
        exit(1);
    }

    // If there’s a "data" object, work on that; otherwise stick to top-level
    struct json_object *dataobj = NULL;
    struct json_object *root = json_root;
    
    if (json_object_object_get_ex(json_root, "data", &dataobj)
        && json_object_get_type(dataobj) == json_type_object)
    {
        root = dataobj;
    }

    // 1) Loop through basic fields
    json_object_object_foreach(root, key, val) {
    
        // Group 1–15: PLU & Basic Info
        if (strcmp(key, "plu_id") == 0)                                 // 1  plu_id – PLU No
            plu_id = json_object_get_int(val);
        else if (strcmp(key, "plu_name") == 0) {                       // 2  plu_name – PLU Name
            const char *s = json_object_get_string(val);
            if (s) { strncpy(plu_name, s, sizeof(plu_name)-1); plu_name[sizeof(plu_name)-1]='\0'; }
        }
        else if (strcmp(key, "plu_code") == 0)                          // 3  plu_code – PLU Code
            strncpy(plu_code, json_object_get_string(val), sizeof(plu_code)-1);
        else if (strcmp(key, "guom") == 0)                              // 4  guom – GUOM (Unit)
            strncpy(guom, json_object_get_string(val), sizeof(guom)-1);
        else if (strcmp(key, "unit_price") == 0) {                        // 5 – Unit Price
	    unit_price = json_object_get_double(val);
	}
	else if (strcmp(key, "spl_up") == 0) {                            // 6 – Special Unit Price
	    const char *s = json_object_get_string(val);
	    if (s) {
		strncpy(spl_up, s, sizeof(spl_up) - 1);
		spl_up[sizeof(spl_up) - 1] = '\0';  // Ensure null-termination
	    }
	}

        else if (strcmp(key, "quantity") == 0)                          // 7  quantity – Quantity
            quantity = json_object_get_int(val);
        else if (strcmp(key, "tare_wt") == 0)                           // 8  tare_wt – Tare Weight
            tare_wt = json_object_get_double(val);
        else if (strcmp(key, "fixed_price") == 0)                       // 9  fixed_price – Fixed Price
            fixed_price = json_object_get_double(val);
        else if (strcmp(key, "packed_date") == 0)                       // 10 packed_date – Packed Date
            strncpy(packed_date, json_object_get_string(val), sizeof(packed_date)-1);
        else if (strcmp(key, "packed_time") == 0)                       // 11 packed_time – Packed Time
            strncpy(packed_time, json_object_get_string(val), sizeof(packed_time)-1);
        else if (strcmp(key, "sellby_date") == 0)                       // 12 sellby_date – Sell By Date
            strncpy(sellby_date, json_object_get_string(val), sizeof(sellby_date)-1);
        else if (strcmp(key, "sellby_time") == 0)                       // 13 sellby_time – Sell By Time
            strncpy(sellby_time, json_object_get_string(val), sizeof(sellby_time)-1);
        else if (strcmp(key, "useby_date") == 0)                        // 14 useby_date – Use By Date
            strncpy(useby_date, json_object_get_string(val), sizeof(useby_date)-1);
        else if (strcmp(key, "useby_time") == 0)                        // 15 useby_time – Use By Time
            strncpy(useby_time, json_object_get_string(val), sizeof(useby_time)-1);

        // Group 16–40: Classification & Header/Footer
        else if (strcmp(key, "plu_minimum") == 0)                       // 16 plu_minimum – PLU Minimum
            plu_minimum = json_object_get_double(val);
        else if (strcmp(key, "plu_target") == 0)                        // 17 plu_target – PLU Target
            plu_target = json_object_get_double(val);
        else if (strcmp(key, "plu_maximum") == 0)                       // 18 plu_maximum – PLU Maximum
            plu_maximum = json_object_get_double(val);
        else if (strcmp(key, "group_no") == 0)                          // 19 group_no – Group No
            group_no = json_object_get_int(val);
        else if (strcmp(key, "group_name") == 0)                        // 20 group_name – Group Name
            strncpy(group_name, json_object_get_string(val), sizeof(group_name)-1);
        else if (strcmp(key, "department_no") == 0)                     // 21 department_no – Department No
            department_no = json_object_get_int(val);
        else if (strcmp(key, "department_name") == 0)                   // 22 department_name – Department Name
            strncpy(department_name, json_object_get_string(val), sizeof(department_name)-1);
        else if (strcmp(key, "tax_no") == 0)                            // 23 tax_no – Tax No
            tax_no = json_object_get_int(val);
        else if (strcmp(key, "tax_name") == 0)                          // 24 tax_name – Tax Name
            strncpy(tax_name, json_object_get_string(val), sizeof(tax_name)-1);
        else if (strcmp(key, "tax_type") == 0)                          // 25 tax_type – Tax Type
            strncpy(tax_type, json_object_get_string(val), sizeof(tax_type)-1);
        else if (strcmp(key, "tax_rate") == 0)                          // 26 tax_rate – Tax Rate
            tax_rate = json_object_get_double(val);
        else if (strcmp(key, "operator_no") == 0)                       // 27 operator_no – Operator No
            operator_no = json_object_get_int(val);
        else if (strcmp(key, "operator_name") == 0)                     // 28 operator_name – Operator Name
            strncpy(operator_name, json_object_get_string(val), sizeof(operator_name)-1);
        else if (strcmp(key, "operator_password") == 0)                 // 29 operator_password – Operator Password
            strncpy(operator_password, json_object_get_string(val), sizeof(operator_password)-1);
        else if (strcmp(key, "header1") == 0)                           // 30 header1 – Header1
            strncpy(header1, json_object_get_string(val), sizeof(header1)-1);
        else if (strcmp(key, "header2") == 0)                           // 31 header2 – Header2
            strncpy(header2, json_object_get_string(val), sizeof(header2)-1);
        else if (strcmp(key, "header3") == 0)                           // 32 header3 – Header3
            strncpy(header3, json_object_get_string(val), sizeof(header3)-1);
        else if (strcmp(key, "header4") == 0)                           // 33 header4 – Header4
            strncpy(header4, json_object_get_string(val), sizeof(header4)-1);
        else if (strcmp(key, "header5") == 0)                           // 34 header5 – Header5
            strncpy(header5, json_object_get_string(val), sizeof(header5)-1);
        else if (strcmp(key, "footer1") == 0)                           // 35 footer1 – Footer1
            strncpy(footer1, json_object_get_string(val), sizeof(footer1)-1);
        else if (strcmp(key, "footer2") == 0)                           // 36 footer2 – Footer2
            strncpy(footer2, json_object_get_string(val), sizeof(footer2)-1);
        else if (strcmp(key, "footer3") == 0)                           // 37 footer3 – Footer3
            strncpy(footer3, json_object_get_string(val), sizeof(footer3)-1);
        else if (strcmp(key, "footer4") == 0)                           // 38 footer4 – Footer4
            strncpy(footer4, json_object_get_string(val), sizeof(footer4)-1);
        else if (strcmp(key, "footer5") == 0)                           // 39 footer5 – Footer5
            strncpy(footer5, json_object_get_string(val), sizeof(footer5)-1);
        else if (strcmp(key, "discount_no") == 0)                       // 40 discount_no – Discount No
            discount_no = json_object_get_int(val);

        // Group 41–60: Promotion, Packaging, Barcode, Discount
        else if (strcmp(key, "discount_name") == 0)                     // 41 discount_name – Discount Name
            strncpy(discount_name, json_object_get_string(val), sizeof(discount_name)-1);
        else if (strcmp(key, "discount_type") == 0)                     // 42 discount_type – Discount Type
            strncpy(discount_type, json_object_get_string(val), sizeof(discount_type)-1);
        else if (strcmp(key, "discount_first_target") == 0)             // 43 discount_first_target – Discount First Target
            discount_first_target = json_object_get_double(val);
        else if (strcmp(key, "discount_first_value") == 0)              // 44 discount_first_value – Discount First Value
            discount_first_value = json_object_get_double(val);
        else if (strcmp(key, "discount_second_target") == 0)            // 45 discount_second_target – Discount Second Target
            discount_second_target = json_object_get_double(val);
        else if (strcmp(key, "discount_second_value") == 0)             // 46 discount_second_value – Discount Second Value
            discount_second_value = json_object_get_double(val);
        else if (strcmp(key, "discount_days") == 0)                     // 47 discount_days – Discount Days
            strncpy(discount_days, json_object_get_string(val), sizeof(discount_days)-1);
        else if (strcmp(key, "discount_start") == 0)                    // 48 discount_start – Discount Start Date/Time
            strncpy(discount_start, json_object_get_string(val), sizeof(discount_start)-1);
        else if (strcmp(key, "discount_end") == 0)                      // 49 discount_end – Discount End Date/Time
            strncpy(discount_end, json_object_get_string(val), sizeof(discount_end)-1);
        else if (strcmp(key, "package_type") == 0)                      // 50 package_type – Package Type
            strncpy(package_type, json_object_get_string(val), sizeof(package_type)-1);
        else if (strcmp(key, "tare_name") == 0)                         // 51 tare_name – Tare Name
            strncpy(tare_name, json_object_get_string(val), sizeof(tare_name)-1);
        else if (strcmp(key, "tare_value") == 0)                        // 52 tare_value – Tare Value
            tare_value = json_object_get_double(val);
        else if (strcmp(key, "storage_temp") == 0)                      // 53 storage_temp – Storage Temperature
            strncpy(storage_temp, json_object_get_string(val), sizeof(storage_temp)-1);
        else if (strcmp(key, "barcode_name") == 0)                      // 54 barcode_name – Barcode Name
            strncpy(barcode_name, json_object_get_string(val), sizeof(barcode_name)-1);
        else if (strcmp(key, "barcode_type") == 0)                      // 55 barcode_type – Barcode Type
            strncpy(barcode_type, json_object_get_string(val), sizeof(barcode_type)-1);
        else if (strcmp(key, "barcode_data") == 0)                      // 56 barcode_data – Barcode Data
            strncpy(barcode_data, json_object_get_string(val), sizeof(barcode_data)-1);
        else if (strcmp(key, "bc_field1") == 0)                         // 57 bc_field1 – Barcode Field 1
            strncpy(bc_field1, json_object_get_string(val), sizeof(bc_field1)-1);
        else if (strcmp(key, "bc_field1_con") == 0)                     // 58 bc_field1_con – Barcode Field 1 Constant
            strncpy(bc_field1_con, json_object_get_string(val), sizeof(bc_field1_con)-1);
        else if (strcmp(key, "bc_field1_shift") == 0)                   // 59 bc_field1_shift – Barcode Field 1 Shift
            strncpy(bc_field1_shift, json_object_get_string(val), sizeof(bc_field1_shift)-1);
        else if (strcmp(key, "bc_field2") == 0)                         // 60 bc_field2 – Barcode Field 2
            strncpy(bc_field2, json_object_get_string(val), sizeof(bc_field2)-1);

        // Group 61–80: Ingredients, Label, Image, Billing
        else if (strcmp(key, "barcode_field2_condition") == 0)         // 61 bc_field2_con – Barcode Field 2 Constant
            strncpy(bc_field2_con, json_object_get_string(val), sizeof(bc_field2_con)-1);
        else if (strcmp(key, "barcode_field2_shift") == 0)             // 62 bc_field2_shift – Barcode Field 2 Shift
            strncpy(bc_field2_shift, json_object_get_string(val), sizeof(bc_field2_shift)-1);
        else if (strcmp(key, "ingredient_no") == 0)                    // 63 ingredient_no – Ingredient No
            ingredient_no = json_object_get_int(val);
        else if (strcmp(key, "ingredient_name") == 0)                  // 64 ingredient_name – Ingredient Name
            strncpy(ingredient_name, json_object_get_string(val), sizeof(ingredient_name)-1);
	else if (strcmp(key, "ingredients_text") == 0)  // 65 – Ingredient Text
    strncpy(ingredients_text, json_object_get_string(val), sizeof(ingredients_text) - 1);


        else if (strcmp(key, "message_no") == 0)                       // 66 message_no – Message No
            message_no = json_object_get_int(val);
        else if (strcmp(key, "message_name") == 0)                     // 67 message_name – Message Name
            strncpy(message_name, json_object_get_string(val), sizeof(message_name)-1);
        else if (strcmp(key, "message_text") == 0)                     // 68 message_text – Message Text
            strncpy(message_text, json_object_get_string(val), sizeof(message_text)-1);
        else if (strcmp(key, "current_net_weight") == 0)               // 69 current_net_weight – Current Net Weight
            current_net_weight = json_object_get_double(val);
        else if (strcmp(key, "current_tare_weight") == 0)              // 70 current_tare_weight – Current Tare Weight
            current_tare_weight = json_object_get_double(val);
        else if (strcmp(key, "current_gross_weight") == 0)             // 71 current_gross_weight – Current Gross Weight
            current_gross_weight = json_object_get_double(val);
        else if (strcmp(key, "weight_or_quantity") == 0)               // 72 weight_or_quantity – Weight or Quantity
            weight_or_quantity = json_object_get_double(val);
        else if (strcmp(key, "actual_unit_price") == 0)                // 73 actual_unit_price – Actual Unit Price
            actual_unit_price = json_object_get_double(val);
        else if (strcmp(key, "image_no") == 0)                         // 74 image_no – Image No
            image_no = json_object_get_int(val);
        else if (strcmp(key, "image_file_name") == 0)                  // 75 image_file_name – Image File Name
            strncpy(image_file_name, json_object_get_string(val), sizeof(image_file_name)-1);
        else if (strcmp(key, "label_datetime") == 0)                   // 76 label_date_time – Label DateTime
            strncpy(label_date_time, json_object_get_string(val), sizeof(label_date_time)-1);
        else if (strcmp(key, "label_design_no") == 0)                  // 77 label_design_no – Label Design No
            label_design_no = json_object_get_int(val);
        else if (strcmp(key, "label_file_name") == 0)                  // 78 label_file_name – Label File Name
            strncpy(label_file_name, json_object_get_string(val), sizeof(label_file_name)-1);
        else if (strcmp(key, "bill_no") == 0)                          // 79 bill_no – Bill No
            bill_no = json_object_get_int(val);
        else if (strcmp(key, "scale_no") == 0)                         // 80 scale_no – Scale No
            strncpy(scale_no, json_object_get_string(val), sizeof(scale_no)-1);

        // Group 81–96: Final Totals & Output Info
        else if (strcmp(key, "scale_name") == 0)                       // 81 scale_name – Scale Name
            strncpy(scale_name, json_object_get_string(val), sizeof(scale_name)-1);
        else if (strcmp(key, "scale_capacity") == 0)                   // 82 scale_capacity – Scale Capacity
            scale_capacity = json_object_get_double(val);
        else if (strcmp(key, "scale_accuracy") == 0)                   // 83 scale_accuracy – Scale Accuracy
            scale_accuracy = json_object_get_double(val);
        else if (strcmp(key, "current_datetime") == 0)                 // 84 current_datetime – Current DateTime
            strncpy(current_datetime, json_object_get_string(val), sizeof(current_datetime)-1);
        else if (strcmp(key, "no_of_items") == 0)                      // 85 no_of_items – No of Items
            no_of_items = json_object_get_int(val);
        else if (strcmp(key, "total_amount") == 0)                     // 86 total_amount – Total Amount
            total_amount = json_object_get_double(val);
        else if (strcmp(key, "total_quantity") == 0)                   // 87 total_quantity – Total Quantity
            total_quantity = json_object_get_double(val);
        else if (strcmp(key, "total_weight") == 0)                     // 88 total_weight – Total Weight
            total_weight = json_object_get_double(val);
        else if (strcmp(key, "total_qty_or_weight") == 0)              // 89 total_qty_or_weight – Total Qty/Weight
            total_qty_or_weight = json_object_get_double(val);
        else if (strcmp(key, "total_tax") == 0)                        // 90 total_tax – Total Tax
            total_tax = json_object_get_double(val);
        else if (strcmp(key, "total_discount") == 0)                   // 91 total_discount – Total Discount
            total_discount = json_object_get_double(val);
        else if (strcmp(key, "today_bill_no") == 0)                    // 92 today_bill_no – Today's Bill No
            today_bill_no = json_object_get_int(val);
        else if (strcmp(key, "total_price") == 0)                      // 93 total_price – Total Price
            total_price = json_object_get_double(val);
        else if (strcmp(key, "uom") == 0) {                             // 94 uom – Unit of Measure
            const char *s = json_object_get_string(val);
            if (s) { strncpy(uom, s, sizeof(uom)-1); uom[sizeof(uom)-1]='\0'; }
        }
        else if (strcmp(key, "barcode_flag") == 0) {                   // 95 barcode_flag – Barcode Flag
            const char *s = json_object_get_string(val);
            if (s) { strncpy(barcode_flag, s, sizeof(barcode_flag)-1); barcode_flag[sizeof(barcode_flag)-1]='\0'; }
        }
        else if (strcmp(key, "bill_text") == 0) {                      // 96 bill_text – Bill Text
            const char *s = json_object_get_string(val);
            if (s) { strncpy(bill_text, s, sizeof(bill_text)-1); bill_text[sizeof(bill_text)-1]='\0'; }
        }
        }
        
	// After processing JSON fields
	if (strcasecmp(uom, "kg") == 0 || strcasecmp(uom, "g") == 0 ||
	    strcasecmp(guom, "kg") == 0 || strcasecmp(guom, "g") == 0) {
	    uom_type = WEIGH;
	} else if (strcasecmp(uom, "pcs") == 0 || strcasecmp(guom, "pcs") == 0) {
	    uom_type = PCS;
	} else {
	    // Handle other units as non-weighing by default
	    uom_type = PCS;
	}

    free(data);
}

//----------- Get_Variable_Text --------------------------------------------------------------------------------

int GetVariableText(unsigned short data_id, char *buf) {
    switch (data_id) {
        // 1–9: PLU Info
        case 1:  sprintf(buf, "%04d", plu_id);                         break; // 1 – PLU No
        case 2:  strcpy(buf, plu_name);                                break; // 2 – PLU Name
        case 3:  strcpy(buf, plu_code);                                break; // 3 – PLU Code
        case 4: {
	    double wgt = weight_or_quantity;
	    if (strcasecmp(uom, "PCS") == 0) {
		strcpy(buf, "PCS");
	    } else {
		// weighing item → g or kg
		if (wgt < 1.0) strcpy(buf, "g");
		else strcpy(buf, "kg");
	    }
	    break;
	}
       case 5:
            snprintf(buf, 64, "%.2f", unit_price);
            break;							// 5 – Unit Price
        case 6:  // Special Unit Price
        {
            // Only use spl_up if it parses to a positive non-zero
            double sp = atof(spl_up);
            if (sp > 0.0) {
                snprintf(buf, 64, "%.2f", sp);
            } else {
                // fallback to unit_price
                snprintf(buf, 64, "%.2f", unit_price);
            }
        }
        break;
        case 7:  sprintf(buf, "%02d", quantity);                       break; // 7 – Unit Price Quantity
        case 8:  sprintf(buf, "%.3f", tare_wt);                        break; // 8 – Tare Weight
        case 9:  sprintf(buf, "%.2f", fixed_price);                    break; // 9 – Fixed Price

        // 10–15: Dates & Times
        case 10: strcpy(buf, packed_date);     break; // 10 – Packed Date
        case 11: strcpy(buf, packed_time);     break; // 11 – Packed Time
        case 12: strcpy(buf, sellby_date);     break; // 12 – Sell By Date
        case 13: strcpy(buf, sellby_time);     break; // 13 – Sell By Time
        case 14: strcpy(buf, useby_date);      break; // 14 – Use By Date
        case 15: strcpy(buf, useby_time);      break; // 15 – Use By Time

        // 16–18: Thresholds
        case 16: sprintf(buf, "%.2f", plu_minimum); break; // 16 – Minimum
        case 17: sprintf(buf, "%.2f", plu_target);  break; // 17 – Target
        case 18: sprintf(buf, "%.2f", plu_maximum); break; // 18 – Maximum

        // 19–22: Group / Department
        case 19: sprintf(buf, "%03d", group_no);        break; // 19 – Group No
        case 20: strcpy(buf, group_name);               break; // 20 – Group Name
        case 21: sprintf(buf, "%02d", department_no);   break; // 21 – Dept No
        case 22: strcpy(buf, department_name);          break; // 22 – Dept Name

        // 23–26: Tax
        case 23: sprintf(buf, "%d", tax_no);            break; // 23 – Tax No
        case 24: strcpy(buf, tax_name);                 break; // 24 – Tax Name
        case 25: strcpy(buf, tax_type);                 break; // 25 – Tax Type
        case 26: sprintf(buf, "%.2f", tax_rate);        break; // 26 – Tax Rate

        // 27–29: Operator
        case 27: sprintf(buf, "%02d", operator_no);     break; // 27 – Operator No
        case 28: strcpy(buf, operator_name);            break; // 28 – Operator Name
        case 29: buf[0] = '\0';                         break; // 29 – Reserved

        // 30–39: Header / Footer
        case 30: strcpy(buf, header1);  break; // 30 – Header1
        case 31: strcpy(buf, header2);  break; // 31 – Header2
        case 32: strcpy(buf, header3);  break; // 32 – Header3
        case 33: strcpy(buf, header4);  break; // 33 – Header4
        case 34: strcpy(buf, header5);  break; // 34 – Header5
        case 35: strcpy(buf, footer1);  break; // 35 – Footer1
        case 36: strcpy(buf, footer2);  break; // 36 – Footer2
        case 37: strcpy(buf, footer3);  break; // 37 – Footer3
        case 38: strcpy(buf, footer4);  break; // 38 – Footer4
        case 39: strcpy(buf, footer5);  break; // 39 – Footer5

        // 40–49: Discount
        case 40: sprintf(buf, "%02d", discount_no);                      break; // 40 – Discount No
        case 41: strcpy(buf, discount_name);                            break; // 41 – Discount Name
        case 42: strcpy(buf, discount_type);                            break; // 42 – Discount Type
        case 43: sprintf(buf, strcmp(guom, "kg") == 0 ? "%.2f" : "%.0f", discount_first_target); break; // 43 – 1st Target
        case 44: sprintf(buf, strcmp(discount_type, "Flat") == 0 ? "Rs. %.2f" : "%.2f%%", discount_first_value); break; // 44 – 1st Value
        case 45: sprintf(buf, strcmp(guom, "kg") == 0 ? "%.2f" : "%.0f", discount_second_target); break; // 45 – 2nd Target
        case 46: sprintf(buf, strcmp(discount_type, "Flat") == 0 ? "Rs. %.2f" : "%.2f%%", discount_second_value); break; // 46 – 2nd Value
        case 47: strcpy(buf, discount_days);                            break; // 47 – Discount Days
        case 48: strcpy(buf, discount_start);                           break; // 48 – Discount Start
        case 49: strcpy(buf, discount_end);                             break; // 49 – Discount End

        // 50–53: Package & Tare
        case 50: strcpy(buf, package_type);                             break; // 50 – Package Type
        case 51: strcpy(buf, tare_name);                                break; // 51 – Tare Name
        case 52: sprintf(buf, "%.2f", tare_value);                      break; // 52 – Tare Value
        case 53: strcpy(buf, storage_temp);                             break; // 53 – Storage Temp

        // 54–62: Barcode
        case 54: strcpy(buf, barcode_name);                             break; // 54 – Barcode Name
        case 55: strcpy(buf, barcode_type);                             break; // 55 – Barcode Type
        case 56: strcpy(buf, barcode_data);                             break; // 56 – Barcode Data
        case 57: strcpy(buf, bc_field1);                                break; // 57 – BC Field 1
        case 58: strcpy(buf, bc_field1_con);                            break; // 58 – BC Field1 Con
        case 59: strcpy(buf, bc_field1_shift);                          break; // 59 – BC Field1 Shift
        case 60: strcpy(buf, bc_field2);                                break; // 60 – BC Field2
        case 61: strcpy(buf, bc_field2_con);                            break; // 61 – BC Field2 Con
        case 62: strcpy(buf, bc_field2_shift);                          break; // 62 – BC Field2 Shift

        // 63–68: Ingredients & Messages
        case 63: sprintf(buf, "%03d", ingredient_no);                   break; // 63 – Ingredient No
        case 64: strcpy(buf, ingredient_name);                          break; // 64 – Ingredient Name
	case 65:
    strcpy(buf, ingredients_text);
    break;


							// 65 – Ingredients Text
        case 66: sprintf(buf, "%03d", message_no);                      break; // 66 – Message No
        case 67: strcpy(buf, message_name);                             break; // 67 – Message Name
        case 68: strcpy(buf, message_text);                             break; // 68 – Message Text

        // 69–73: Weights / Prices
        case 69: sprintf(buf, "%.3f", current_net_weight);              break; // 69 – Net Wt
        case 70: sprintf(buf, "%.3f", current_tare_weight);             break; // 70 – Tare Wt
        case 71: sprintf(buf, "%.3f", current_gross_weight);            break; // 71 – Gross Wt
        case 72: {
	    double tf = weight_or_quantity;
	    if (uom_type == WEIGH) {
		if (lbl_wtgrams && tf <= 1.0) {
		    int grams = (int)(tf * 1000);
		    snprintf(buf, 64, "%d", grams);  // grams, integer
		} else {
		    snprintf(buf, 64, "%.3f", tf);   // kg, 3 decimal places
		}
	    } else {
		snprintf(buf, 64, "%.0f", tf);  // non-weighing → quantity, no decimal
	    }
	} break;
        case 73:
            snprintf(buf, 64, "%.2f", actual_unit_price);
            break;				

        // 74–78: Label & Image
        case 74: sprintf(buf, "%02d", image_no);                        break; // 74 – Image No
        case 75: strcpy(buf, image_file_name);                          break; // 75 – Image Filename
        case 76: strcpy(buf, label_date_time);                          break; // 76 – Label Datetime
        case 77: sprintf(buf, "%02d", label_design_no);                break; // 77 – Label Design No
        case 78: strcpy(buf, label_file_name);                          break; // 78 – Label Filename

        // 79–80: Bill & Scale
        case 79: sprintf(buf, "%05d", bill_no);                         break; // 79 – Bill No
        case 80: strcpy(buf, scale_no);                                 break; // 80 – Scale No

        // 81–83: Scale Info
        case 81: strcpy(buf, scale_name);                               break; // 81 – Scale Name
        case 82: sprintf(buf, "%.0f", scale_capacity);                 break; // 82 – Capacity
        case 83: sprintf(buf, "%.3f", scale_accuracy);                 break; // 83 – Accuracy

        // 84: Current DateTime
        case 84: strcpy(buf, current_datetime);                         break; // 84 – Current DateTime

        // 85–92: Totals
        case 85: sprintf(buf, "%02d", no_of_items);                    break; // 85 – No. of Items
        case 86: sprintf(buf, "%.2f", total_amount);                   break; // 86 – Total Amount
        case 87: sprintf(buf, "%.0f", total_quantity);                 break; // 87 – Total Qty
        case 88: sprintf(buf, "%.3f", total_weight);                   break; // 88 – Total Weight
        case 89: sprintf(buf, total_quantity > 0 ? "%.0f" : "%.3f", total_quantity > 0 ? total_quantity : total_weight); break; // 89 – Total Qty or Wt
        case 90: sprintf(buf, "%.2f", total_tax);                      break; // 90 – Total Tax
        case 91: sprintf(buf, "%.2f", total_discount);                 break; // 91 – Total Discount
        case 92: sprintf(buf, "%05d", today_bill_no);                  break; // 92 – Today Bill No

        // 93–96: Other
        case 93: sprintf(buf, "%.2f", total_price); break; // 93 – Final Price
        case 94: {
	    if (strcasecmp(uom, "PCS") == 0) {
		strcpy(buf, "PCS");
	    } else {
		strcpy(buf, "kg");
	    }
	    break;
	}

        case 95: strcpy(buf, barcode_flag);                            break; // 95 – Barcode Flag
        case 96: if (bill_text[0]) strcpy(buf, bill_text); else buf[0] = '\0'; break; // 96 – Bill Text

        default: buf[0] = '\0'; return -1;
    }
    return 0;
}


// ------------ STUBS & GLOBALS ----------------------------------------------------------------------------------

// Dummy RTC
typedef struct { int dd,mm,yyyy,hr,min,sec,dow; } RTC_CFG;
void RTC_Get(RTC_CFG *r) { r->dd=1; r->mm=1; r->yyyy=2025; r->hr=12; r->min=0; r->sec=0; r->dow=3; }

// Format flags & bill timestamp
int date_format=0, time_format=0, lbl_date_format=0, lbl_time_format=0;
int bill_dd=1,bill_mm=1,bill_yyyy=2025, bill_hr=0,bill_min=0,bill_sec=0;
int tare_no=0;

// Packed-date stub
time_t packed_date_to_secs(unsigned d,unsigned t){ (void)d; (void)t; return time(NULL); }

// Stubs for linkage
void GetItemInfoByIndex(int idx, char *out_plu, float *out_qty_wt, int *out_uom) {
    (void)idx;
    strcpy(out_plu, "000");
    *out_qty_wt = 0.0f;
    *out_uom    = 0;
}
float ConvertToGrams(float w) { return w * 1000.0f; }

// -------- External placeholders -------------------------------------------------------------------------------------

extern char   barcode_data[64];
extern int    plu_id, department_no, no_of_items, operator_no, group_no;
extern char plu_code[32], guom[32], scale_no[32], scale_name[64], barcode_flag[32], bill_text[128];
extern double total_amount, total_weight, total_quantity, total_tax, total_discount, total_price;
extern double unit_price, weight_or_quantity, tare_wt, current_gross_weight;

// Helper: parse date/time strings
static void parse_dt(const char *ds, const char *ts, struct tm *out) {
    memset(out, 0, sizeof *out);
    if (!strptime(ds, "%Y%m%d", out)) {
        RTC_Get((RTC_CFG*)out);
    }
    if (!strptime(ts, "%H%M%S", out)) {
        out->tm_hour = out->tm_min = out->tm_sec = 0;
    }
}

//------------ GetBarcode Data -------------------------------------------------------------------------------------------

int GetBarcodeData(char *bdp, const char *bt) {
    char t[64]; size_t i = 0;
    RTC_CFG rtc;
    struct tm dt;

    bdp[0] = '\0';
    while (i < strlen(barcode_data)) {
        if (barcode_data[i] == ' ') { i++; continue; }
        // width (1-99)
        int w = 0;
        while (isdigit(barcode_data[i])) w = w*10 + (barcode_data[i++] - '0');
        if (!w) w = 1;
        char c = barcode_data[i++];
        t[0] = '\0';

        switch (c) {
            case 'A': sprintf(t, "%0*.0f", w, total_amount*100); break;  // 86 – TOTAL_AMOUNT
            case 'B': sprintf(t, "%0*d", w, bill_dd);        break;  // 79 – BILL NO
            case 'b': sprintf(t, "%0*d", w, bill_mm);        break;  // 92 – Today Bill no
            case 'C': sprintf(t, "%.*s", w, plu_code);       break;  // 3  – PLU CODE
            case 'D': sprintf(t, "%0*d", w, department_no);  break;  // 21 – DEPARTMENT NO
            case 'E': sprintf(t, "%0*.0f", w, total_weight*1000); break; // 88 – TOTAL_WEIGHT
            case 'F': sprintf(t, "%.*s", w, barcode_flag);   break;  // 95 – FLAG
            case 'G': sprintf(t, "%0*d", w, group_no);       break;  // 19 – GROUP NO
            case 'H': sprintf(t, "%0*.0f", w, total_quantity); break; // 87 – TOTAL_QUANTITY
            case 'I': sprintf(t, "%0*.0f", w, total_tax*100); break; // 90 – TOTAL_TAX
            case 'J': sprintf(t, "%0*.0f", w, total_discount*100); break; // 91 – TOTAL_DISCOUNT
            case 'K': RTC_Get(&rtc); sprintf(t, "%02d%02d%02d", rtc.dd, rtc.mm, rtc.yyyy%100); break; // 84 – CURRENT DATE
            case 'k': sprintf(t, "%02d%02d%02d", bill_dd, bill_mm, bill_yyyy%100); break; // 76 – Label date
            case 'L': sprintf(t, "%0*d", w, plu_id);        break;  // 1  – PLU NO
            case 'M': sprintf(t, "%.*s", w, guom);           break;  // 4  – gUOM
            case 'N': sprintf(t, "%0*d", w, no_of_items);   break;  // 85 – NO OF ITEMS
            case 'n': sprintf(t, "%*s", w, scale_no);       break;  // 80 – Machine No
            case 'O': sprintf(t, "%0*d", w, operator_no);   break;  // 27 – OPERATOR NO
            case 'P': sprintf(t, "%0*.0f", w, total_price*100); break; // 93 – TOTAL PRICE
            case 'Q': if (!strcmp(guom,"pcs")) sprintf(t, "%0*.0f", w, weight_or_quantity); else sprintf(t, "%0*d", w, 0); break; // 72 – QUANTITY ONLY
            case 'R': sprintf(t, "%0*d", w, 0);             break;  // 40 – DISCOUNT NO
            case 'S': {  
	    double price = unit_price;  
	    // if spl_up is non‐empty and parses to >0, use it instead  
	    if (spl_up[0]) {  
		double sp = atof(spl_up);  
		if (sp > 0) price = sp;  
	    }  
	    // now print price ×100 as integer  
	    snprintf(t, sizeof t, "%0*.0f", w, price * 100.0);  
	    break;  
	}
	case 's': {  
	    // same logic but only 4‐digit field  
	    double price = unit_price;  
	    if (spl_up[0]) {  
		double sp = atof(spl_up);  
		if (sp > 0) price = sp;  
	    }  
	    snprintf(t, sizeof t, "%0*.0f", w, price * 100.0);  
	    break;  
	}

            case 'T': sprintf(t, "%0*d", w, 0);             break;  // 23 – TAX NO
            case 't': sprintf(t, "%*.*s", w, w, bill_text);  break;  // 96 – Bill Text
            case 'U':  // UNIT PRICE always  
	    snprintf(t, sizeof t, "%0*.0f", w, unit_price * 100.0);  
	    break;

		    case 'V': case 'v': sprintf(t, "%0*.0f", w, weight_or_quantity*1000); break; // 72 – Special checksum
            case 'W': if (!strcmp(guom,"kg")) sprintf(t, "%0*.0f", w, weight_or_quantity*1000); else sprintf(t, "%0*d", w, 0); break;  // 72 – WEIGHT ONLY
            case 'w': sprintf(t, "%0*.0f", w, tare_wt*1000); break;  // 8 – TARE WEIGHT
            case 'X': sprintf(t, "%0*.0f", w, weight_or_quantity*1000); break; // 72 – WEIGHT OR QUANTITY
            case 'x': sprintf(t, "%0*.0f", w, current_gross_weight*1000); break; // 71 – GROSS WEIGHT
            case 'Y': RTC_Get(&rtc); sprintf(t, "%02d%02d%02d", rtc.hr, rtc.min, rtc.sec); break;  // 84 – CURRENT TIME
            case 'y': sprintf(t, "%02d%02d%02d", bill_hr, bill_min, bill_sec); break; // 76 – LABEL TIME
            case 'Z': sprintf(t, "%.*s", w, scale_name);     break;  // 81 – MACHINE NAME
            case 'z': sprintf(t, "%0*d", w, tare_no);       break;  // 52 – TARE LINK NO
            case '%': {
    char lit[64];
    int  p = 0;
    // skip any spaces
    while (i < (int)strlen(barcode_data) && barcode_data[i]==' ')
        i++;
    // copy up to next space or end
    while (i < (int)strlen(barcode_data)
        && barcode_data[i] != ' '
        && p < (int)(sizeof(lit)-1))
    {
        lit[p++] = barcode_data[i++];
    }
    lit[p] = '\0';
    // truncate to w chars of the literal:
    snprintf(t, sizeof t, "%.*s", w, lit);
} break;

            case '{': case '/': case '}': case '[': case '\\': case ']': { parse_dt(
                    (c=='{'?packed_date:(c=='/'?sellby_date:useby_date)),
                    (c=='['?packed_time:(c=='\\'?sellby_time:useby_time)),
                    &dt);
                strftime(t, sizeof t,
                    (c=='{'||c=='/'||c=='}')?(lbl_date_format?"%d%m%Y":"%d%m%y"):(lbl_time_format?"%H%M%S":"%H%M"),
                    &dt);
            } break; // 10–15 – DATE/TIME
            case '*': {
                int cw=w, xw=atoi(&barcode_data[i++]);
                for(int m=0; m<no_of_items; m++){
                    char pu[32]; float wt; int u;
                    GetItemInfoByIndex(m, pu, &wt, &u);
                    sprintf(t, "%*s,%0*.0f\r\n", cw, pu, xw,
                        (!strcmp(guom,"KG")?ConvertToGrams(wt):wt));
                    strcat(bdp, t);
                }
                continue;
            } // 1,72 – PLU*WT/Q
        default:
        {
            // Treat any other character as literal
            char lit[2] = { c, '\0' };
            // pad/truncate to width w
            snprintf(t, sizeof t, "%*s", w, lit);
        }
        break;
     }
     strcat(bdp, t);
 }
    return 0;
}

// ------------- Position & Style Helpers ----------------------------------------------------------

void set_absolute_position(int prn, int x_dots, int y_dots) {
    uint8_t cmd_x[4] = { ESC, '$', x_dots & 0xFF, (x_dots >> 8) & 0xFF };
    uint8_t cmd_y[4] = { ESC, 'Y', y_dots & 0xFF, (y_dots >> 8) & 0xFF };
    write_all(prn, cmd_x, sizeof(cmd_x));
    write_all(prn, cmd_y, sizeof(cmd_y));
}
void set_printer_rotation(int fd, int angle) {
    uint8_t cmd[] = { ESC, 'V', (uint8_t)angle };
    write_all(fd, cmd, sizeof(cmd));
}

void select_font(int fd, int font) {
    uint8_t cmd[] = { ESC, 'M', (uint8_t)font };
    write_all(fd, cmd, sizeof(cmd));
}

void set_text_size(int fd, float h, float w) {
    int dh = (int)(h * DOTS_PER_MM + 0.5f);
    int dw = (int)(w * DOTS_PER_MM + 0.5f);
    uint8_t cmd[] = { GS, '!', ((dh/8)<<4)|(dw/8) };
    write_all(fd, cmd, sizeof(cmd));
}

int compute_text_width(const char *text, int font, float xmul) {
    int base_width = (font == 1) ? 12 : 9; // font 1 = 12-dot, font 2 = 9-dot
    int len = strlen(text);
    return (int)(base_width * xmul * len);
}

int compute_text_height(int lines, float spacing_mm, float ymul) {
    return (int)(spacing_mm * lines * ymul * DOTS_PER_MM);
}

void parse_mode(const char *mode, int *bold, int *underline, int *invert) {
    *bold = *underline = *invert = 0;
    if (strchr(mode, 'E')) *bold = 1;
    if (strchr(mode, 'U')) *underline = 1;
    if (strchr(mode, 'I')) *invert = 1;
}


// ------ send_text() -------------------------------------------------------------------

void send_text(int prn,
               float x, float y,
               int font,
               float xmul, float ymul,
               const char *text,
               int data_len,
               int offset,
               char justify,
               int lines,
               float line_spacing_mm,
               int angle,
               const char *mode)
{
    if (!text || lines < 1) return;

    int xmag = fmaxf(1, fminf(6, roundf(xmul)));
    int ymag = fmaxf(1, fminf(6, roundf(ymul)));

    int base_w = (font == 1 ? 12 : 9);
    int base_h = (font == 1 ? 24 : 17);
    uint8_t esc_m = (font == 1 ? 0 : 1);

    int full = strlen(text);
    if (offset >= full) return;
    const char *ptext = text + offset;
    int len = full - offset;
    if (data_len > 0 && len > data_len) len = data_len;

    int char_width = base_w * xmag;
    int char_height = base_h * ymag;

    int text_width_dots = char_width * len;
    int box_width_dots  = (data_len > 0 ? char_width * data_len : text_width_dots);
    int spacing = (int)(line_spacing_mm * DOTS_PER_MM + 0.5f);
    if (spacing < char_height) spacing = char_height;

    int xpos = (int)((x + lbl_x_offset) * DOTS_PER_MM + 0.5f);
    int ypos = (int)((y + lbl_y_offset) * DOTS_PER_MM + 0.5f);

    // Justification
    if (justify == 'C') {
        xpos += (box_width_dots - text_width_dots) / 2;
    } else if (justify == 'R') {
        xpos += (box_width_dots - text_width_dots);
    }

    // Rotation code
    uint8_t esc_t = 0;
    if (angle == 90) esc_t = 1;
    else if (angle == 180) esc_t = 2;
    else if (angle == 270) esc_t = 3;

    // Set orientation
    write_all(prn, (uint8_t[]){ ESC, 'T', esc_t }, 3);
    write_all(prn, (uint8_t[]){ ESC, 'M', esc_m }, 3);
    write_all(prn, (uint8_t[]){ GS, '!', ((xmag - 1) << 4) | (ymag - 1) }, 3);
    write_all(prn, (uint8_t[]){ ESC, '3', (uint8_t)spacing }, 3);

    // Modes
    if (strchr(mode, 'E')) write_all(prn, (uint8_t[]){ ESC, 'E', 1 }, 3);
    if (strchr(mode, 'U')) write_all(prn, (uint8_t[]){ ESC, '-', 1 }, 3);
    if (strchr(mode, 'I')) write_all(prn, (uint8_t[]){ GS, 'B', 1 }, 3);

    const char *line = ptext;
    for (int i = 0; i < lines && line; i++) {
        const char *e = strchr(line, '\n');
        int this_len = e ? (int)(e - line) : strlen(line);
        int y_i = ypos + i * spacing;

        // Measure line dimensions based on magnification
        int dx = char_width * this_len;
        int dy = char_height;

        // Add a safe margin to prevent clipping (especially for descenders)
        int margin_x = 2 * xmag;  // Add left/right padding
        int margin_y = 2 * ymag;  // Add top/bottom padding

        dx += margin_x;
        dy += margin_y;

        int x0 = xpos;
        int y0 = y_i;
        int win_dx, win_dy;

        if (angle == 90) {
            y0 = y0 - (dx - 1);
            win_dx = spacing * lines + margin_y;
            win_dy = dx;
        } else if (angle == 180) {
            x0 = x0 - (dx - 1);
            y0 = y0 - (dy - 1);
            win_dx = dx;
            win_dy = spacing * lines + margin_y;
        } else if (angle == 270) {
            x0 = x0 - (spacing * lines - 1);
            win_dx = spacing * lines + margin_y;
            win_dy = dx;
        } else {
            win_dx = dx;
            win_dy = spacing * lines + margin_y;
        }

        // Set ESC W window with full coverage
        write_all(prn, (uint8_t[]){
            ESC, 'W',
            lo(x0), hi(x0),
            lo(y0), hi(y0),
            lo(win_dx), hi(win_dx),
            lo(win_dy), hi(win_dy)
        }, 10);

        // Print the text
        write_all(prn, (const uint8_t *)line, this_len);
        line = e ? e + 1 : NULL;
    }


    // Reset
    write_all(prn, (uint8_t[]){ LF }, 1);
    write_all(prn, (uint8_t[]){ ESC, 'E', 0 }, 3);
    write_all(prn, (uint8_t[]){ ESC, '-', 0 }, 3);
    write_all(prn, (uint8_t[]){ GS, 'B', 0 }, 3);
    write_all(prn, (uint8_t[]){ GS, '!', 0 }, 3);
    write_all(prn, (uint8_t[]){ ESC, '3', 32 }, 3);
}


// -------- send_barcode() ----------------------------------------------------------------

void send_barcode(int prn,
                  float x, float y,
                  float module_width_mm,
                  float bar_height_mm,
                  const char *data,
                  const char *orig_type,
                  char hri_pos,
                  const char *barcode_name, int angle, char justify,
                  const char *fld1, const char *cond1, const char *shift1,
                  const char *fld2, const char *cond2, const char *shift2)
{
    // 1) Clear any text mode
    uint8_t clear_mode[] = {
        ESC, 'M', 0,
        GS, '!', 0,
        ESC, 'E', 0,
        ESC, 'a', 0,
        ESC, '3', 24
    };
    write_all(prn, clear_mode, sizeof(clear_mode));

    // 2) Set full window (ESC W) — REQUIRED to avoid clipping
    write_all(prn, (uint8_t[]){
        ESC, 'W',
        0x00, 0x00,                      // X start = 0
        0x00, 0x00,                      // Y start = 0
        lo((int)(lbl_width_mm * DOTS_PER_MM)),   // Width
        hi((int)(lbl_width_mm * DOTS_PER_MM)),
        lo((int)(lbl_height_mm * DOTS_PER_MM)),  // Height
        hi((int)(lbl_height_mm * DOTS_PER_MM))
    }, 10);

    int xpos = (int)((x + lbl_x_offset) * DOTS_PER_MM + 0.5f);
    int barcode_h_dots = (int)(bar_height_mm * DOTS_PER_MM + 0.5f);
    int ypos = (int)((y + lbl_y_offset) * DOTS_PER_MM + 0.5f + barcode_h_dots);

    int data_len = strlen(data);
    int module_width_dots = (int)(module_width_mm * DOTS_PER_MM + 0.5f);
    int barcode_width_dots = data_len * module_width_dots;

    // Adjust X based on justification
    if (justify == 'C') {
        xpos -= barcode_width_dots / 2;
    } else if (justify == 'R') {
        xpos -= barcode_width_dots;
    }

    // === ANGLE HANDLING ===
    uint8_t esc_t = 0;
    if (angle == 90) {
        esc_t = 1;
        int temp = xpos;
        xpos = ypos;
        ypos = (int)(lbl_height_mm * DOTS_PER_MM) - temp - barcode_width_dots;
    } else if (angle == 180) {
        esc_t = 2;
        xpos = (int)(lbl_width_mm * DOTS_PER_MM) - xpos - barcode_width_dots;
        ypos = (int)(lbl_height_mm * DOTS_PER_MM) - ypos - barcode_h_dots;
    } else if (angle == 270) {
        esc_t = 3;
        int temp = xpos;
        xpos = (int)(lbl_width_mm * DOTS_PER_MM) - ypos - barcode_h_dots;
        ypos = temp;
    }

    // Set printer rotation
    uint8_t escT_cmd[3] = { ESC, 'T', esc_t };
    write_all(prn, escT_cmd, sizeof(escT_cmd));

    // Position
    uint8_t pos_cmd[8] = {
        ESC, '$', lo(xpos), hi(xpos),
        GS,  '$', lo(ypos), hi(ypos)
    };
    write_all(prn, pos_cmd, sizeof(pos_cmd));

    // Barcode width and height
    uint8_t wcmd[3] = { GS, 'w', (uint8_t)module_width_dots };
    uint8_t hcmd[3] = { GS, 'h', (uint8_t)barcode_h_dots };
    write_all(prn, wcmd, sizeof(wcmd));
    write_all(prn, hcmd, sizeof(hcmd));

    // HRI font & position
    uint8_t hri_font_cmd[3] = { GS, 'f', 1 };
    uint8_t hri_pos_cmd[3] = {
        GS, 'H',
        (hri_pos == 'B' ? 2 :
         hri_pos == 'A' ? 1 :
         hri_pos == '2' ? 3 : 0)
    };
    write_all(prn, hri_font_cmd, sizeof(hri_font_cmd));
    write_all(prn, hri_pos_cmd, sizeof(hri_pos_cmd));

    // === Decide barcode type ===
    size_t L = strlen(data);
    bool all_digits = true;
    for (size_t i = 0; i < L; i++) {
        if (!isdigit((unsigned char)data[i])) {
            all_digits = false;
            break;
        }
    }

    if (all_digits && L == 12) {
        // EAN-13
        uint8_t hdr[] = { GS, 'k', 2 };
        write_all(prn, hdr, sizeof(hdr));
        write_all(prn, (const uint8_t*)data, 12);
        uint8_t term = 0x00;
        write_all(prn, &term, 1);
    } else if (strcmp(orig_type, "QRCODE") == 0) {
        if (L == 0 || L > 120) return;
        uint8_t cmd1[] = { GS,'(','k',3,0,49,69,49 };
        uint8_t cmd2[] = { GS,'(','k',3,0,49,67,6  };
        uint16_t sl = L + 3;
        uint8_t pl = sl & 0xFF, ph = sl >> 8;
        uint8_t cmd3[] = { GS,'(','k',pl,ph,49,80,48 };
        write_all(prn, cmd1, sizeof(cmd1));
        write_all(prn, cmd2, sizeof(cmd2));
        write_all(prn, cmd3, sizeof(cmd3));
        write_all(prn, (const uint8_t*)data, L);
    } else {
        char data_buf[260];
        if (L + 1 > sizeof(data_buf)) return;
        memcpy(data_buf, data, L + 1);

        if (all_digits && (L % 2 != 0)) {
            memmove(data_buf + 1, data_buf, L + 1);
            data_buf[0] = '0';
            L++;
        }

        char subset;
        if (all_digits) {
            subset = 'C';
        } else {
            bool has_lower_or_punct = false;
            for (size_t i = 0; i < L; i++) {
                unsigned char c = data_buf[i];
                if (islower(c) || ispunct(c)) {
                    has_lower_or_punct = true;
                    break;
                }
            }
            subset = has_lower_or_punct ? 'B' : 'A';
        }

        char send_buf[264];
        int dlen = snprintf(send_buf, sizeof(send_buf), "{%c%s", subset, data_buf);
        uint8_t hdr[4] = { GS, 'k', 73, (uint8_t)dlen };
        write_all(prn, hdr, 4);
        write_all(prn, (const uint8_t*)send_buf, dlen);
    }

    // Restore to safe mode after barcode
    uint8_t reset[] = {
        ESC, 'M', 0,
        GS,  '!', 0,
        ESC, 'E', 0
    };
    write_all(prn, reset, sizeof(reset));
}




// ------- send_rectangel() -----------------------------------------------------------------------

void send_rectangle(int prn, float x_mm, float y_mm,
                    float w_mm, float h_mm,
                    float th_mm, int angle,
                    char mode, char printstatus)
{
    if (!CheckPrintStatus(printstatus)) return;

    // Adjust X/Y by label offsets
    float x0_mm = x_mm + lbl_x_offset;
    float y0_mm = y_mm + lbl_y_offset;

    // Rotate logic applied via coordinate adjustment (not ESC T)
    float xloc, yloc, dx, dy;

    if (angle == 90) {
        xloc = x0_mm;
        yloc = y0_mm - w_mm;
        dx = h_mm;
        dy = w_mm;
    } else if (angle == 180) {
        xloc = x0_mm - w_mm;
        yloc = y0_mm - h_mm;
        dx = w_mm;
        dy = h_mm;
    } else if (angle == 270) {
        xloc = x0_mm - h_mm;
        yloc = y0_mm;
        dx = h_mm;
        dy = w_mm;
    } else {
        xloc = x0_mm;
        yloc = y0_mm;
        dx = w_mm;
        dy = h_mm;
    }

    // Convert mm to dots
    int x0 = (int)(xloc * DOTS_PER_MM + 0.5f);
    int y0 = (int)(yloc * DOTS_PER_MM + 0.5f);
    int x1 = (int)((xloc + dx) * DOTS_PER_MM + 0.5f) - 1;
    int y1 = (int)((yloc + dy) * DOTS_PER_MM + 0.5f) - 1;
    int lwidth = (int)(th_mm * DOTS_PER_MM + 0.5f);
    int invert = (mode == 'I') ? 1 : 0;

    // Set full window
    uint16_t full_x = (uint16_t)(lbl_width_mm * DOTS_PER_MM + 0.5f);
    uint16_t full_y = (uint16_t)(lbl_height_mm * DOTS_PER_MM + 0.5f);
    uint8_t fullwin[10] = {
        ESC, 'W', 0, 0, 0, 0,
        lo(full_x), hi(full_x),
        lo(full_y), hi(full_y)
    };
    write_all(prn, fullwin, sizeof(fullwin));

    // Angle always 0 (we handled rotation in coordinates)
    write_all(prn, (uint8_t[]){ ESC, 'T', 0 }, 3);
    write_all(prn, (uint8_t[]){ GS, 'B', (uint8_t)invert }, 3);

    // Draw rectangle
    uint8_t cmd[] = {
        FS, 'R',
        lo(x0), hi(x0),
        lo(y0), hi(y0),
        lo(x1), hi(x1),
        lo(y1), hi(y1),
        (uint8_t)lwidth
    };
    write_all(prn, cmd, sizeof(cmd));
}

// ------ send_read_response ----------------------------------------------------------------------

bool send_read_response(int prn, const char *expected, int timeout_ms) {
    char buf[256];
    int elapsed = 0;

    // clamp timeout
    if (timeout_ms < 5)    timeout_ms = 5;
    if (timeout_ms > 5000) timeout_ms = 5000;

    while (elapsed < timeout_ms) {
        // try a nonblocking read
        int n = read(prn, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            // strip trailing CR/LF
            while (n > 0 && (buf[n-1]=='\n' || buf[n-1]=='\r')) {
                buf[--n] = '\0';
            }
            if (strcmp(buf, expected) == 0) {
                return true;
            }
        }
        usleep(1000);  // wait 1 ms
        elapsed++;
    }
    return false;
}


// ----- send_bitmap_data ------------------------------------------------------------------------

void send_bitmap_data(int prn,
                      float x_mm, float y_mm,
                      int angle,
                      int xmag, int ymag,
                      float width_mm, float height_mm,
                      char type,
                      const char *mode,
                      FILE *image_fp)
{
    if (!image_fp) {
        fprintf(stderr, "[ERROR] Image file pointer is NULL\n");
        return;
    }

    int raw_w = (int)(width_mm * DOTS_PER_MM + 0.5f);
    int raw_h = (int)(height_mm * DOTS_PER_MM + 0.5f);
    int img_w = raw_w * xmag;
    int img_h = raw_h * ymag;
    int bytes_per_row = (img_w + 7) / 8;
    int expected_bytes = bytes_per_row * img_h;

    fprintf(stderr, "[DEBUG] Width mm: %.2f Height mm: %.2f => raw_w: %d raw_h: %d\n",
            width_mm, height_mm, raw_w, raw_h);
    fprintf(stderr, "[DEBUG] xmag: %d ymag: %d => img_w: %d img_h: %d\n",
            xmag, ymag, img_w, img_h);
    fprintf(stderr, "[DEBUG] Expected bytes: %d (bytes_per_row: %d)\n",
            expected_bytes, bytes_per_row);

    uint8_t *img = calloc(1, expected_bytes);
    if (!img) {
        fprintf(stderr, "[ERROR] Memory allocation failed\n");
        return;
    }

    size_t read = fread(img, 1, expected_bytes, image_fp);
    fprintf(stderr, "[DEBUG] Successfully read %zu bytes of image data\n", read);

    // If image is empty, skip
    int has_black = 0;
    for (int i = 0; i < expected_bytes; ++i) {
        if (img[i]) {
            has_black = 1;
            break;
        }
    }
    if (!has_black) {
        fprintf(stderr, "[WARN] Image contains only white pixels, skipping\n");
        free(img);
        return;
    }

    // Transpose for rotation = 0
    if (angle == 0) {
        int t_bytes_per_row = (img_h + 7) / 8;
        int t_expected_bytes = t_bytes_per_row * img_w;
        uint8_t *transposed = calloc(1, t_expected_bytes);
        if (!transposed) {
            free(img);
            return;
        }

        for (int y = 0; y < img_h; ++y) {
            for (int x = 0; x < img_w; ++x) {
                int src_byte = y * bytes_per_row + (x / 8);
                int src_bit = 7 - (x % 8);
                int bit = (img[src_byte] >> src_bit) & 1;

                int dst_byte = x * t_bytes_per_row + (y / 8);
                int dst_bit = 7 - (y % 8);
                if (bit) transposed[dst_byte] |= (1 << dst_bit);
            }
        }

        free(img);
        img = transposed;
        bytes_per_row = t_bytes_per_row;
        expected_bytes = t_expected_bytes;

        int tmp = img_w;
        img_w = img_h;
        img_h = tmp;
    }

    uint8_t esc_t = 0;
    if (angle == 90) esc_t = 1;
    else if (angle == 180) esc_t = 2;
    else if (angle == 270) esc_t = 3;

    int xpos = (int)((x_mm + lbl_x_offset) * DOTS_PER_MM + 0.5f);
    int ypos = (int)((y_mm + lbl_y_offset) * DOTS_PER_MM + 0.5f);
    int x0 = xpos, y0 = ypos;
    int win_w = img_w, win_h = img_h;

    if (angle == 90) {
        y0 -= (img_w - 1);
        win_w = img_h;
        win_h = img_w;
    } else if (angle == 180) {
        x0 -= (img_w - 1);
        y0 -= (img_h - 1);
    } else if (angle == 270) {
        x0 -= (img_h - 1);
        win_w = img_h;
        win_h = img_w;
    }

    // Validate window
    int max_x = (int)(lbl_width_mm * DOTS_PER_MM);
    int max_y = (int)(lbl_height_mm * DOTS_PER_MM);
    if (x0 < 0 || y0 < 0 || x0 + win_w > max_x || y0 + win_h > max_y) {
        fprintf(stderr, "[WARN] Image outside label area, adjusting\n");
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 + win_w > max_x) win_w = max_x - x0;
        if (y0 + win_h > max_y) win_h = max_y - y0;
    }

    fprintf(stderr, "[DEBUG] Final print position: x=%d y=%d angle=%d win_w=%d win_h=%d\n",
            x0, y0, angle, win_w, win_h);

    write_all(prn, (uint8_t[]){ ESC, 'W', lo(x0), hi(x0), lo(y0), hi(y0), lo(win_w), hi(win_w), lo(win_h), hi(win_h) }, 10);
    write_all(prn, (uint8_t[]){ ESC, 'T', esc_t }, 3);

    uint8_t inv = 0, enh = 0, und = 0;
    if (mode) {
        if (strchr(mode, 'I')) inv = 1;
        if (strchr(mode, 'E')) enh = 1;
        if (strchr(mode, 'U')) und = 1;
    }
    write_all(prn, (uint8_t[]){ GS, 'B', inv }, 3);
    write_all(prn, (uint8_t[]){ ESC, 'E', enh }, 3);
    write_all(prn, (uint8_t[]){ ESC, '-', und }, 3);

    write_all(prn, (uint8_t[]){ GS, '$', 0, 0 }, 4);

    uint8_t magnify = ((ymag - 1) << 4) | (xmag - 1);
    write_all(prn, (uint8_t[]){ GS, 'v', '0', magnify, lo(bytes_per_row), hi(bytes_per_row), lo(img_h), hi(img_h) }, 8);

    write_all(prn, img, expected_bytes);
    fsync(prn);

    write_all(prn, (uint8_t[]){ ESC, 'T', 0 }, 3);
    write_all(prn, (uint8_t[]){ GS, 'B', 0 }, 3);
    write_all(prn, (uint8_t[]){ ESC, 'E', 0 }, 3);
    write_all(prn, (uint8_t[]){ ESC, '-', 0 }, 3);

    free(img);
}



//-------------- Decode backslash-escaped binary image string ----------------------------------------

void decode_escaped_binary(FILE *f, FILE *out, int expected_bytes) {
    int ch, count = 0;
    while ((ch = fgetc(f)) != EOF && count < expected_bytes) {
        if (ch == '\\') {
            int h1 = fgetc(f);
            int h2 = fgetc(f);
            if (h1 == EOF || h2 == EOF) break;
            char hex[3] = { h1, h2, 0 };
            uint8_t val = (uint8_t)strtol(hex, NULL, 16);
            fputc(val, out);
            count++;
        } else if (ch != '\n' && ch != '\r') {
            fputc((uint8_t)ch, out);
            count++;
        }
    }
    fprintf(stderr, "[DEBUG] Decoded %d bytes from escaped image data\n", count);
}


// ------------- Send_Circle --------------------------------------------------------------------------------------

void send_circle(int prn, float x, float y, float radius, float thickness, char mode, char printstatus)
{
    if (!CheckPrintStatus(printstatus)) return;

    // Convert mm to dots with offsets
    int xloc = (int)((x + lbl_x_offset) * DOTS_PER_MM + 0.5f);
    int yloc = (int)((y + lbl_y_offset) * DOTS_PER_MM + 0.5f);
    int radius_dots = (int)(radius * DOTS_PER_MM + 0.5f);
    int thick_dots = (int)(thickness * DOTS_PER_MM + 0.5f);
    int invert = (mode == 'I') ? 1 : 0;

    // Full window like rectangle
    uint16_t full_x = (uint16_t)(lbl_width_mm * DOTS_PER_MM + 0.5f);
    uint16_t full_y = (uint16_t)(lbl_height_mm * DOTS_PER_MM + 0.5f);
    uint8_t fullwin[10] = {
        ESC, 'W', 0, 0, 0, 0,
        lo(full_x), hi(full_x),
        lo(full_y), hi(full_y)
    };
    write_all(prn, fullwin, sizeof(fullwin));

    // Set rotation to 0
    write_all(prn, (uint8_t[]){ ESC, 'T', 0 }, 3);

    // Set invert mode
    write_all(prn, (uint8_t[]){ GS, 'B', (uint8_t)invert }, 3);

    // Now send the circle
    uint8_t cmd[12];
    int i = 0;
    cmd[i++] = FS; cmd[i++] = 'c';
    cmd[i++] = lo(xloc); cmd[i++] = hi(xloc);
    cmd[i++] = lo(yloc); cmd[i++] = hi(yloc);
    cmd[i++] = (uint8_t)radius_dots;
    cmd[i++] = (uint8_t)thick_dots;

    write_all(prn, cmd, i);
}

// --------------------------------------------------------------------------------------------------

uint8_t *job_buf = NULL;
size_t job_len = 0, job_cap = 0;


void ensure_capacity(size_t more) {
    if (job_len + more > job_cap) {
        size_t newcap = job_cap ? job_cap * 2 : INITIAL_CAP;
        while (newcap < job_len + more) newcap *= 2;
        job_buf = realloc(job_buf, newcap);
        job_cap = newcap;
    }
}

void buffer_data(const void *data, size_t len) {
    ensure_capacity(len);
    memcpy(job_buf + job_len, data, len);
    job_len += len;
}

// --------- int main ----------------------------------------------------------------------

int main(int argc, char **argv) {

if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("Essae WSLPR Driver Version: %s\n", DRIVER_VERSION);
        return 0;
    }

    if (argc == 3) {
        // CLI mode
        return convert_label(argv[1], argv[2]);
    }
    else if (argc == 1) {
	// 1. Open & configure the scale serial port (OPTIONAL)
	weight_fd = open("/dev/ttyS4", O_RDWR | O_NOCTTY | O_SYNC);
	if (weight_fd < 0) {
	    perror("Warning: scale not connected (/dev/ttyS4)");
	    weight_fd = -1;  // mark as unavailable
	} else {
	    struct termios tty;
	    memset(&tty, 0, sizeof(tty));
	    if (tcgetattr(weight_fd, &tty) != 0) {
		perror("tcgetattr for scale");
		close(weight_fd);
		weight_fd = -1;
	    } else {
		cfsetospeed(&tty, B9600);
		cfsetispeed(&tty, B9600);
		tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
		tty.c_iflag &= ~(IXON | IXOFF | IXANY);
		tty.c_lflag = 0;
		tty.c_oflag = 0;
		tty.c_cc[VMIN]  = 0;
		tty.c_cc[VTIME] = 5;  // 0.5s read timeout
		if (tcsetattr(weight_fd, TCSANOW, &tty) != 0) {
		    perror("tcsetattr for scale");
		    close(weight_fd);
		    weight_fd = -1;
		}
	    }
	}
       // 2. Start TCP server on port 8888
        int server_fd = setup_server_socket(PORT);
        printf("Listening on TCP port %d...\n", PORT);

        // 3. Accept loop—always listening, never closing the port
        while (1) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int *pclient = malloc(sizeof(int));
            if (!pclient) {
                perror("malloc");
                // Sleep a bit to avoid busy looping if malloc keeps failing
                sleep(1);
                continue;
            }

            *pclient = accept(server_fd,
                              (struct sockaddr *)&client_addr,
                              &addrlen);
            if (*pclient < 0) {
                perror("accept");
                free(pclient);
                continue;
            }

            // Launch a detached thread to handle this client
            pthread_t tid;
            if (pthread_create(&tid, NULL, client_thread, pclient) != 0) {
                perror("pthread_create");
                close(*pclient);
                free(pclient);
                continue;
            }
            pthread_detach(tid);
        }

    }
    else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s config.json label.LFT   (CLI mode)\n", argv[0]);
        fprintf(stderr, "  %s                         (Server mode)\n", argv[0]);
        return 1;
    }
    return 0;
}

//-------- convert label ----------------------------------------------------------------------------------

int convert_label(const char *config_path, const char *lft_path) {
    // ─── Step 1: Load JSON file for basic fields (not barcode) ──────
    load_json_data(config_path);
    if (json_root == NULL) {
        fprintf(stderr, "Error: failed to parse JSON in %s\n", config_path);
        return 1;
    }

    // ─── Step 1.a: Extract actual_unit_price and spl_up from JSON ───
    {
        struct json_object *d = NULL, *val = NULL;

        if (json_object_object_get_ex(json_root, "data", &d)) {
            if (json_object_object_get_ex(d, "actual_unit_price", &val)) {
                actual_unit_price = atof(json_object_get_string(val));
            }
            if (json_object_object_get_ex(d, "spl_up", &val)) {
                unit_price = atof(json_object_get_string(val));
            }
        }
    }

    // ─── Step 2: Override weight/quantity from scale if needed ──────
    if (uom_type == WEIGH) {
        char rawbuf[64] = {0};
        double kg = 0.0;

        // Send RD_WEIGHT (0x05) to scale
        unsigned char rd_cmd = 0x05;
        if (write(weight_fd, &rd_cmd, 1) < 0) {
            perror("Error writing RD_WEIGHT to scale port");
        } else {
            usleep(200000);  // wait for response

            int n = read(weight_fd, rawbuf, sizeof(rawbuf) - 1);
            if (n > 0) {
                rawbuf[n] = '\0';
                kg = atof(rawbuf);
            } else {
                fprintf(stderr, "Warning: scale RD_WEIGHT returned no data.\n");
                kg = 0.0;
            }
        }

        current_gross_weight = kg;     // Data ID 71
        weight_or_quantity   = kg;     // Data ID 72
    }

    // ─── Step 3: Load LFT content from SQLite slot ──────────────────
    int slot = atoi(lft_path);  // Note: lft_path is actually slot number string

    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc = sqlite3_open(LFT_DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error: cannot open LFT database: %s\n", sqlite3_errmsg(db));
        return 2;
    }

    const char *sql = "SELECT content FROM lft_files WHERE slot = ?";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, slot);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "Error: no LFT file found for slot %d\n", slot);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 2;
    }

    const void *blob = sqlite3_column_blob(stmt, 0);
    int blob_size = sqlite3_column_bytes(stmt, 0);

    // Write blob to temp file
    const char *temp_lft_path = "/var/tmp/server_selected.lft";
    FILE *f = fopen(temp_lft_path, "w+b");
    if (!f) {
        perror("Error opening temp LFT file");
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 2;
    }
    fwrite(blob, 1, blob_size, f);
    fclose(f);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // ─── Step 4: Reopen LFT file for parsing and printing ───────────
    f = fopen(temp_lft_path, "r");
    if (!f) {
        perror("Error reopening temp LFT file");
        return 2;
    }

    
    const char *portname = "/dev/ttyS0";
    int fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("opening serial port");
        fclose(f);
        return 3;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        fclose(f);
        return 4;
    }
    cfsetospeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &tty);

uint8_t init_seq[] = { ESC, '@' };
    write_all(fd, init_seq, sizeof(init_seq));

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='#' || line[0]=='@' || line[0]=='\n')
            continue;


 // -------- ~S: define label size & page window in mm --------------------------------------------------

        if (strncmp(line,"~S",2)==0) {
            float w,h,g; int no;
            if (sscanf(line+3,"%f,%f,%f,%d",&w,&h,&g,&no)>=2) {
                lbl_width_mm  = w;
                lbl_height_mm = h;
                uint16_t x_d = (uint16_t)(w*DOTS_PER_MM + 0.5f);
                uint16_t y_d = (uint16_t)(h*DOTS_PER_MM + 0.5f);

                // FS L: label size
                write_all(fd, (uint8_t[]){ FS,'L',
                    lo(x_d),hi(x_d), lo(y_d),hi(y_d)
                }, 6);
                // ESC L: enter page mode
                write_all(fd, (uint8_t[]){ ESC,'S' }, 2);
                // ESC W: set window = entire label
                write_all(fd, (uint8_t[]){ ESC,'W',
                    0,0, 0,0,
                    lo(x_d),hi(x_d), lo(y_d),hi(y_d)
                },10);
                // no hardware offset: y=0.0 → top
                lbl_x_offset = 0.0f;
                lbl_y_offset = 0.0f;
            }
        }
 // ------- ~s: line spacing (mm) ----------------------------------------------------------------------------
 
	else if (strncmp(line, "~s", 2) == 0) {
	    float sp_mm;
	    // parse spacing in mm
	    if (sscanf(line + 3, "%f", &sp_mm) == 1) {
		// convert mm to dots
		int n = (int)(sp_mm * DOTS_PER_MM + 0.5f);
		// ESC 3 n
		uint8_t cmd[3] = { 0x1B, '3', (uint8_t)n };
		write_all(fd, cmd, sizeof(cmd));
	    }
	}

 // ------ ~A: clear area ------------------------------------------------------------------------------------
 
        else if (strncmp(line, "~A", 2) == 0) {
    float x,y,dx,dy; char mode;
    if (sscanf(line+3, "%f,%f,%f,%f,%c", &x,&y,&dx,&dy,&mode) >= 5) {
        // 1) Compute in dots
        int x0 = (int)((x + lbl_x_offset) * DOTS_PER_MM + 0.5f);
        int y0 = (int)((y + lbl_y_offset) * DOTS_PER_MM + 0.5f);
        int dx0 = (int)(dx * DOTS_PER_MM + 0.5f);
        int dy0 = (int)(dy * DOTS_PER_MM + 0.5f);

        // 2) ESC W: set page‐mode window to just that rectangle
        uint8_t win_cmd[10] = {
            ESC, 'W',
            lo(x0), hi(x0),
            lo(y0), hi(y0),
            lo(dx0), hi(dx0),
            lo(dy0), hi(dy0)
        };
        write_all(fd, win_cmd, sizeof(win_cmd));

        // 3) CAN: clear *all* data in that window
        uint8_t can = 0x18;
        write_all(fd, &can, 1);

        // 4) Restore the window to full‐label (your existing ESC W)
        uint16_t full_x = (uint16_t)(lbl_width_mm  * DOTS_PER_MM + 0.5f);
        uint16_t full_y = (uint16_t)(lbl_height_mm * DOTS_PER_MM + 0.5f);
        uint8_t fullwin[10] = {
            ESC, 'W',
            0,0, 0,0,
            lo(full_x), hi(full_x),
            lo(full_y), hi(full_y)
        };
        write_all(fd, fullwin, sizeof(fullwin));
    }
}
        
// ------- ~T Fixed Text ----------------------------------------------------------------------------

else if (strncmp(line, "~T", 2) == 0) {
    float x, y, xm, ym, spacing;
    int angle, font, len, offset, lines;
    char justify, mode_str[4] = "", prnstatus = '1';
    char raw[512], decoded[512];

    char *p = line + 3;

    // Trim trailing whitespace/newlines
    for (int i = strlen(p) - 1; i >= 0 && isspace((unsigned char)p[i]); --i)
        p[i] = '\0';

    // Extract print status (last char)
    char *c = strrchr(p, ',');
    if (c && strlen(c + 1) == 1 && isdigit((unsigned char)*(c + 1))) {
        prnstatus = *(c + 1);
        *c = '\0';
    }

    // Manually extract last 6 fields (mode, spacing, lines, justify, offset, len)
    char *fields[13];
    int field_count = 0;

    for (char *token = p; token && field_count < 13; ) {
        // Handle escaped comma
        char *comma = token;
        int escaped = 0;

        while (*comma) {
            if (*comma == '\\' && comma[1] == ',') {
                comma += 2; // skip escaped comma
                escaped = 1;
                continue;
            }
            if (*comma == ',') break;
            comma++;
        }

        if (*comma == ',') {
            *comma = '\0';
            fields[field_count++] = token;
            token = comma + 1;
        } else {
            fields[field_count++] = token;
            break;
        }
    }

    if (field_count < 13) continue;

    // Now decode the text field
    char *s = fields[6], *d = decoded;
    while (*s) {
        if (s[0] == '\\') {
            if (s[1] == 'n') { *d++ = '\n'; s += 2; }
            else if (s[1] == ',') { *d++ = ','; s += 2; }
            else if (s[1] == '\\') { *d++ = '\\'; s += 2; }
            else { *d++ = *s++; }
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';

    // Assign other parsed fields
    x = atof(fields[0]);
    y = atof(fields[1]);
    angle = atoi(fields[2]);
    font = atoi(fields[3]);
    xm = atof(fields[4]);
    ym = atof(fields[5]);
    len = atoi(fields[7]);
    offset = atoi(fields[8]);
    justify = fields[9][0];
    lines = atoi(fields[10]);
    spacing = atof(fields[11]);
    strncpy(mode_str, fields[12], 3); mode_str[3] = '\0';

    if (!CheckPrintStatus(prnstatus)) continue;

    send_text(fd, x, y, font, xm, ym, decoded, len, offset, justify, lines, spacing, angle, mode_str);
}



// ----------- ~V Variable Text ----------------------------------------------------------------------------------

else if (strncmp(line, "~V", 2) == 0) {
    float x, y, xm, ym, spacing;
    int angle, font, len, offset, lines, text_start = 0;
    char justify, mode_str[4] = "", prnstatus = '1';
    char id[32] = "", raw[512] = "", decoded[512] = "", actual[512] = "";
    char *p = line + 3, *c;

    // Trim trailing newline/space
    for (int i = strlen(p) - 1; i >= 0 && isspace((unsigned char)p[i]); --i)
        p[i] = '\0';

    // Check for print status at end
    c = strrchr(p, ',');
    if (c && strlen(c + 1) == 1 && isdigit((unsigned char)*(c + 1))) {
        prnstatus = *(c + 1);
        *c = '\0';
    }

    // Parse fixed parameters
    c = strrchr(p, ','); mode_str[0] = *(c + 1); *c = 0;
    c = strrchr(p, ','); spacing = atof(c + 1); *c = 0;
    c = strrchr(p, ','); lines = atoi(c + 1); *c = 0;
    c = strrchr(p, ','); justify = *(c + 1); *c = 0;
    c = strrchr(p, ','); offset = atoi(c + 1); *c = 0;
    c = strrchr(p, ','); len = atoi(c + 1); *c = 0;

    // Parse main params and extract ID + raw
    if (sscanf(p, "%f,%f,%d,%d,%f,%f,%31[^,],%n", &x, &y, &angle, &font, &xm, &ym, id, &text_start) < 7) {
        continue;  // invalid line
    }

    strncpy(raw, p + text_start, sizeof(raw) - 1);
    raw[sizeof(raw)-1] = '\0';

    if (!CheckPrintStatus(prnstatus)) continue;

    // Decode escape sequences
    char *s = raw, *d = decoded;
    while (*s) {
        if (*s == '\\') {
            if (s[1] == 'n') { *d++ = '\n'; s += 2; }
            else if (s[1] == ',') { *d++ = ','; s += 2; }
            else if (s[1] == '\\') { *d++ = '\\'; s += 2; }
            else { *d++ = *s++; }
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';

    // Get actual variable
    actual[0] = '\0';
    if (isdigit((unsigned char)id[0]) && GetVariableText(atoi(id), actual) == 0) {
        // OK
    } else if (json_root) {
        struct json_object *datao, *valo;
        if (json_object_object_get_ex(json_root, "data", &datao) &&
            json_object_object_get_ex(datao, id, &valo)) {
            snprintf(actual, sizeof(actual), "%s", json_object_get_string(valo));
        } else {
            strcpy(actual, decoded);  // fallback
        }
    } else {
        strcpy(actual, decoded);  // fallback
    }

    send_text(fd, x, y, font, xm, ym, actual, len, offset, justify, lines, spacing, angle, mode_str);
}



// ------ Barcode ~B handler (JSON-driven) ------------------------------------------------------------------


else if (strncmp(line, "~B", 2) == 0) {
    float x, y, module_width_mm, bar_height_mm;
    int angle, font, offset, data_length;
    char justify = 'N', hri = 'N', mode = 'W';

    // Parse full barcode line — we ignore .LFT barcode data + type
    if (sscanf(line + 3,
        "%f,%f,%d,%d,%f,%f,%*[^,],%d,%d,%c,%*[^,],%c,%c,%*[^,\r\n]",
        &x, &y,
        &angle, &font,
        &module_width_mm, &bar_height_mm,
        &data_length, &offset,
        &justify, &hri, &mode
    ) != 11) {
        fprintf(stderr, "Invalid ~B line format: %s\n", line);
        continue;
    }

    // Get barcode from JSON using selected barcode number
    int data_id = gui_data_id;

    // ✅ FIXED range check (was: if (data_id < 1 || data_id > num_json_barcodes))
    if (data_id < 1 || data_id > 99) {
        fprintf(stderr, "Invalid barcode number: %d\n", data_id);
        continue;
    }

    char bdata[128] = {0}, btype[16] = {0}, bname[16] = {0};
    char fld1[16] = {0}, cond1[8] = {0}, shift1[4] = {0};
    char fld2[16] = {0}, cond2[8] = {0}, shift2[4] = {0};

    if (LoadDBBarcodeRecord(data_id,
        bdata, btype, bname,
        fld1, cond1, shift1,
        fld2, cond2, shift2) != 0)
    {
        fprintf(stderr, "Failed to load barcode #%d from DB\n", data_id);
        continue;
    }

    // Build actual barcode data
    strncpy(barcode_data, bdata, sizeof(barcode_data)-1);
    barcode_data[sizeof(barcode_data)-1] = '\0';

    char pattern[256] = {0};
    if (GetBarcodeData(pattern, btype) != 0) {
        fprintf(stderr, "Error building barcode %d\n", data_id);
        continue;
    }

    // Truncate to requested length
    if (data_length > 0 && data_length < (int)strlen(pattern)) {
        pattern[data_length] = '\0';
    }

    printf("Barcode[%d] pattern: %s (type=%s, HRI=%c, len=%d)\n",
           data_id, pattern, btype, hri, data_length);

    // Send barcode to printer
    send_barcode(fd, x, y, module_width_mm, bar_height_mm,
                 pattern, btype, hri, bname,
                 angle, justify,
                 fld1, cond1, shift1,
                 fld2, cond2, shift2);

    // ─── Optional field labels below barcode ─────────────────
    int modw = (int)(module_width_mm * DOTS_PER_MM + 0.5f);

    int should_print(const char *cond, float weight_or_quantity, int quantity) {
        if (strcmp(cond, "No") == 0 || strcmp(cond, "Any") == 0) return 1;
        if (strcmp(cond, "Weight") == 0 && weight_or_quantity > 0.0f) return 1;
        if (strcmp(cond, "Quantity") == 0 && quantity > 0) return 1;
        return 0;
    }

    int compute_shift(const char *sh, float x, float module_width_mm, const char *pattern) {
        int n = sh[1] - '0';
        int modw = (int)(module_width_mm * DOTS_PER_MM + 0.5f);
        if (sh[0] == 'L')
            return (int)(x * DOTS_PER_MM) - n * modw;
        else
            return (int)((x + module_width_mm * strlen(pattern) / (float)DOTS_PER_MM) * DOTS_PER_MM) + n * modw;
    }

    if (should_print(cond1, weight_or_quantity, quantity) && fld1[0]) {
        int sx = compute_shift(shift1, x, module_width_mm, pattern);
        set_absolute_position(fd, sx / (float)DOTS_PER_MM, y + bar_height_mm + 2.0f);
        write_all(fd, (const uint8_t*)fld1, strlen(fld1));
    }

    if (should_print(cond2, weight_or_quantity, quantity) && fld2[0]) {
        int sx = compute_shift(shift2, x, module_width_mm, pattern);
        set_absolute_position(fd, sx / (float)DOTS_PER_MM, y + bar_height_mm + 4.0f);
        write_all(fd, (const uint8_t*)fld2, strlen(fld2));
    }
}


// ------ ~R Rectangle ------------------------------------------------------------------


else if (strncmp(line, "~R", 2) == 0) {
    float x = 0, y = 0, angle = 0, dx = 0, dy = 0, th = 0;
    char mode = 'W', status = '1';  // Default printstatus = '1'

    // Parse the line with safe fallback
    int count = sscanf(line + 3, "%f,%f,%f,%f,%f,%f,%c,%c",
                       &x, &y, &angle, &dx, &dy, &th, &mode, &status);

    if (count >= 7) {
        send_rectangle(fd, x, y, dx, dy, th, (int)angle, mode, status);
    }
}

// ------ ~C Circle ------------------------------------------------------------------

else if (strncmp(line, "~C", 2) == 0) {
    float x, y, r, t;
    char mode = 'W', printstatus = '1';
    int num = sscanf(line + 3, "%f,%f,%f,%f,%c,%c", &x, &y, &r, &t, &mode, &printstatus);
    if (num >= 5) {
        send_circle(fd, x, y, r, t, mode, printstatus);
    }
}

// ------ ~c Escape Codes ------------------------------------------------------------------

else if (strncmp(line, "~c", 2) == 0) {
    // ~c – Send raw ESC/POS codes (comma-separated integers)
    uint8_t esc_bytes[64];
    int value, n = 0;
    const char *p = line + 3;

    while (*p && n < 64) {
        if (sscanf(p, "%d", &value) == 1) {
            esc_bytes[n++] = (uint8_t)value;
        }

        // Skip to next comma
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }

    if (n > 0) {
        write_all(fd, esc_bytes, n);
    }
}

// ------ ~d Bitmap Data  ------------------------------------------------------------------

else if (strncmp(line, "~d", 2) == 0) {
    float x, y, width, height;
    int angle, xmag, ymag;
    char type, mode[4] = "W", prnstatus = '1';
    char *fields[11];
    int i = 0;

    char *p = line + 3;
    char *token = strtok(p, ",");
    while (token && i < 11) {
        fields[i++] = token;
        token = strtok(NULL, ",");
    }
    if (i < 9) continue;

    x = atof(fields[0]);
    y = atof(fields[1]);
    angle = atoi(fields[2]);
    xmag = atoi(fields[3]);
    ymag = atoi(fields[4]);
    width = atof(fields[5]);
    height = atof(fields[6]);
    type = fields[7][0];
    strncpy(mode, fields[8], 3);
    mode[3] = '\0';
    if (i > 9 && isdigit(fields[9][0])) prnstatus = fields[9][0];

    if (!CheckPrintStatus(prnstatus)) continue;

    int img_w = (int)(width * DOTS_PER_MM + 0.5f) * xmag;
    int img_h = (int)(height * DOTS_PER_MM + 0.5f) * ymag;
    int bytes_per_row = (img_w + 7) / 8;
    int total_bytes = bytes_per_row * img_h;

    int first = fgetc(f);
    ungetc(first, f);

    FILE *imgfp = NULL;
    if (first == '\\') {
        FILE *tmpfp = tmpfile();
        if (!tmpfp) continue;
        decode_escaped_binary(f, tmpfp, total_bytes);
        rewind(tmpfp);
        imgfp = tmpfp;
    } else {
        imgfp = f;
    }

    send_bitmap_data(fd, x, y, angle, xmag, ymag, width, height, type, mode, imgfp);

    if (imgfp != f) fclose(imgfp);
}


// ------ ~Y Delay  ------------------------------------------------------------------

else if (strncmp(line, "~Y", 2) == 0) {
    // ~Y – Delay in milliseconds (5–5000 ms)
    int delay_ms;
    if (sscanf(line + 3, "%d", &delay_ms) == 1) {
        if (delay_ms < 5) delay_ms = 5;
        else if (delay_ms > 5000) delay_ms = 5000;
        usleep(delay_ms * 1000);
    }
}

// ------ ~I Intensity  ------------------------------------------------------------------

else if (strncmp(line, "~I", 2) == 0) {
    int level;
    // parse level after “~I,”
    if (sscanf(line + 3, "%d", &level) == 1) {
        // clamp to 60–140%
        if (level < 60) level = 60;
        if (level > 140) level = 140;
        // DC2 '∼' n  ← this is 0x12, 0x7E, level
        uint8_t cmd[3] = { 0x12, 0x7E, (uint8_t)level };
        write_all(fd, cmd, sizeof(cmd));
    }
}

// ------ ~e Read Response  ------------------------------------------------------------------

else if (strncmp(line, "~e", 2) == 0) {
    char mode = '\0';
    char expected[128] = {0};
    int timeout_ms = 0;

    // parse:  single-char mode, up to 127-byte expected string, integer timeout
    if (sscanf(line + 3, " %c , %127[^,] , %d",
               &mode, expected, &timeout_ms) >= 3) {
        // invoke helper
        bool got = send_read_response(fd, expected, timeout_ms);
        // optional debug:
        // fprintf(stderr, "~e: waited %dms for \"%s\" → %s\n",
        //         timeout_ms, expected, got ? "OK" : "TIMEOUT");
    }
}


 // ------- ~P: print & exit page mode ------------------------------------------------------------
 
         else if (strncmp(line,"~P",2)==0){
            int copies; char dir;
            if (sscanf(line+3,"%d,%c",&copies,&dir)!=2) copies=1;
            // streaming print direction if you like:
            write_all(fd, (uint8_t[]){ ESC,'{', (uint8_t)(dir=='U'?1:0) },3);
            for(int i=0;i<copies;i++)
                write_all(fd, (uint8_t[]){ GS,0x0C },2);  // GS FF
            write_all(fd, (uint8_t[]){ ESC,'S' },2);       // ESC S
        }
        
// -------------------------------------------------------------------------------------------------

   }

    if (!json_root) {
    json_object_put(json_root);
    json_root = NULL;
}

	fclose(f);
	close(fd);
	return 0;
}

// ------------- End Of The Driver Code -----------------------------------------------------------------

