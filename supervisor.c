/**
 * \file supervisor.c
 * \brief Supervisor implementation.
 * \author Marek Svepes <svepemar@fit.cvut.cz>
 * \author Tomas Cejka <cejkat@cesnet.cz>
 * \date 2013
 * \date 2014
 */
/*
 * Copyright (C) 2013-2014 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#define _GNU_SOURCE
#ifdef nemea_plugin
   #include "./ncnemea/ncnemea.h"
#endif

#include "supervisor.h"
#include "supervisor_api.h"
#include "internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <libtrap/trap.h>

#define TRAP_PARAM   "-i" ///< Interface parameter for libtrap
#define DEFAULT_MAX_RESTARTS_PER_MINUTE   3  ///< Maximum number of module restarts per minute
#define SERVICE_IFC_CONN_ATTEMPTS_LIMIT   3 // Maximum count of connection attempts to service interface
#define MAX_SERVICE_IFC_CONN_FAILS   3

#define MODULES_UNIXSOCKET_PATH_FILENAME_FORMAT   "/tmp/trap-localhost-%s.sock" ///< Modules output interfaces socket, to which connects service thread.
#define DEFAULT_DAEMON_SERVER_SOCKET   DEFAULT_PATH_TO_SOCKET  ///<  Daemon server socket
#define DEFAULT_NETCONF_SERVER_SOCKET   "/tmp/netconf_supervisor.sock"  ///<  Netconf server socket

#define DEFAULT_PATH_TO_CONFIGSS   DEFAULT_PATH_TO_CONFIGS


#define INIT_TMP_LOG_PATH   "/tmp/sup_tmp_log_file"
#define INIT_TMP_DEBUG_LOG_PATH   "/tmp/sup_tmp_debug_log_file"
#define NETCONF_DEFAULT_LOGSDIR_PATH   "/tmp/netconf_supervisor_logs/"
#define DAEMON_DEFAULT_LOGSDIR_PATH   "/tmp/daemon_supervisor_logs/"
#define INTERACTIVE_DEFAULT_LOGSDIR_PATH   "/tmp/interactive_supervisor_logs/"
#define BACKUP_FILE_PREFIX   SUP_TMP_DIR
#define BACKUP_FILE_SUFIX   "_sup_backup_file.xml"

#define RET_ERROR   -1
#define MAX_NUMBER_SUP_CLIENTS   5
#define NUM_SERVICE_IFC_PERIODS   30
#define SERVICE_WAIT_BEFORE_TIMEOUT 25000

#define SERVICE_GET_COM 10
#define SERVICE_SET_COM 11
#define SERVICE_OK_REPLY 12

/*
 * Time in micro seconds the service thread spends sleeping after each period.
 * (the period means all tasks service thread has to complete - restart and stop modules according to their enable flag,
 * receive their statistics etc.)
 */
#define SERVICE_THREAD_SLEEP_IN_MICSEC 1500000

/*
 * Time in micro seconds between sending SIGINT and SIGKILL to running modules.
 * Service thread sends SIGINT to stop running module, after time defined by this constant it checks modules status
 * and if the module is still running service thread sends SIGKILL to stop it.
 */
#define SERVICE_WAIT_FOR_MODULES_TO_FINISH 500000

/*******GLOBAL VARIABLES*******/
running_module_t *running_modules = NULL;  ///< Information about running modules

unsigned int running_modules_array_size = 0;  ///< Current size of running_modules array.
unsigned int loaded_modules_cnt = 0; ///< Current number of loaded modules.
long int last_total_cpu_usage = 0; // Variable with total cpu usage of whole operating system

pthread_mutex_t running_modules_lock; ///< mutex for locking counters
int service_thread_continue = FALSE; ///< condition variable of main loop of the service_thread
int max_restarts_per_minute_config = DEFAULT_MAX_RESTARTS_PER_MINUTE;

modules_profile_t *first_profile_ptr = NULL;
modules_profile_t *actual_profile_ptr = NULL;
unsigned int loaded_profile_cnt = 0;

pthread_t service_thread_id; ///< Service thread identificator.
pthread_t netconf_server_thread_id;

time_t sup_init_time = 0;
int service_stop_all_modules = FALSE;

// supervisor flags
int supervisor_initialized = FALSE;
int service_thread_initialized = FALSE;
int daemon_mode_initialized = FALSE;
int logs_paths_initialized = FALSE;
int modules_logs_path_initialized = FALSE;
int daemon_flag = FALSE;      // --daemon
int netconf_flag = FALSE;

char *templ_config_file = NULL;
char *gener_config_file = NULL;
char *running_config_file = NULL;
char *config_files_path = NULL;
char *socket_path = NULL;
char *logs_path = NULL;

char *statistics_file_path = NULL;
char *module_event_file_path = NULL;
char *supervisor_debug_log_file_path = NULL;
char *supervisor_log_file_path = NULL;

server_internals_t *server_internals = NULL;

/**************************************/

// Returns absolute path of the file / directory passed in file_name parameter
char *get_absolute_file_path(char *file_name)
{
   if (file_name == NULL) {
      return NULL;
   }

   static char absolute_file_path[PATH_MAX];
   memset(absolute_file_path, 0, PATH_MAX);

   if (realpath(file_name, absolute_file_path) == NULL) {
      return NULL;
   }
   return absolute_file_path;
}

// Creates backup file path using configuration file name
char *create_backup_file_path()
{
   uint x = 0;
   char *absolute_config_file_path = NULL;
   uint32_t letter_sum = 0;
   char *buffer = NULL;

   // Get absolute path of the configuration file
   absolute_config_file_path = get_absolute_file_path(templ_config_file);
   if (absolute_config_file_path == NULL) {
      return NULL;
   }

   // Add up all letters of the absolute path multiplied by their index
   for (x = 0; x < strlen(absolute_config_file_path); x++) {
      letter_sum += absolute_config_file_path[x] * (x+1);
   }

   // Create path of the backup file: "/tmp/sup_tmp_dir/" + letter_sum + "_sup_backup.xml"
   if (asprintf(&buffer, "%s/%d%s", BACKUP_FILE_PREFIX, letter_sum, BACKUP_FILE_SUFIX) < 0) {
      return NULL;
   }

   return buffer;
}

void create_shutdown_info(char **backup_file_path)
{
   FILE *info_file_fd = NULL;
   char *info_file_name = NULL;

   if (asprintf(&info_file_name, "%s_info", *backup_file_path) < 0) {
      return;
   }

   info_file_fd = fopen(info_file_name, "w");
   if (info_file_fd == NULL) {
      NULLP_TEST_AND_FREE(info_file_name)
      return;
   }

   fprintf(info_file_fd, "Supervisor shutdown info:\n==========================\n\n");
   fprintf(info_file_fd, "Supervisor package version: %s\n", sup_package_version);
   fprintf(info_file_fd, "Supervisor git version: %s\n", sup_git_version);
   fprintf(info_file_fd, "Started: %s", ctime(&sup_init_time));
   fprintf(info_file_fd, "Actual date and time: %s\n", get_formatted_time());
   fprintf(info_file_fd, "Number of modules in configuration: %d\n", loaded_modules_cnt);
   fprintf(info_file_fd, "Number of running modules: %d\n", service_check_modules_status());
   fprintf(info_file_fd, "Logs directory: %s\n", get_absolute_file_path(logs_path));
   fprintf(info_file_fd, "Configuration file: %s\n\n", get_absolute_file_path(templ_config_file));
   fprintf(info_file_fd, "Run supervisor with this configuration file to load generated backup file. It will connect to running modules.\n");

   NULLP_TEST_AND_FREE(info_file_name)
   fclose(info_file_fd);
}

void print_xmlDoc_to_stream(xmlDocPtr doc_ptr, FILE *stream)
{
   if (doc_ptr != NULL && stream != NULL) {
      xmlChar *formated_xml_output = NULL;
      int size = 0;
      xmlDocDumpFormatMemory(doc_ptr, &formated_xml_output, &size, 1);
      if (formated_xml_output == NULL) {
         return;
      } else {
         fprintf(stream, "%s\n", formated_xml_output);
         fflush(stream);
         xmlFree(formated_xml_output);
      }
   }
}

char *get_formatted_time()
{
   time_t rawtime;
   static char formatted_time_buffer[DEFAULT_SIZE_OF_BUFFER];

   memset(formatted_time_buffer,0,DEFAULT_SIZE_OF_BUFFER);
   time(&rawtime);

   sprintf(formatted_time_buffer, "%s", ctime(&rawtime));
   formatted_time_buffer[strlen(formatted_time_buffer) - 1] = 0;

   return formatted_time_buffer;
}

char **parse_module_params(const uint32_t module_idx, uint32_t *params_num)
{
   uint32_t params_arr_size = 5, params_cnt = 0;
   char **params = (char **) calloc(params_arr_size, sizeof(char *));
   char buffer[DEFAULT_SIZE_OF_BUFFER];
   memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER * sizeof(char));
   uint32_t x = 0, y = 0, act_param_len = 0;
   int params_len = strlen(running_modules[module_idx].module_params);

   if (params_len < 1) {
      VERBOSE(MODULE_EVENT, "%s [WARNING] Empty string in \"%s\" params element.", get_formatted_time(), running_modules[module_idx].module_name);
      goto err_cleanup;
   }

   for (x = 0; x < params_len; x++) {
      switch(running_modules[module_idx].module_params[x]) {
      /* parameter in apostrophes */
      case '\'':
      {
         if (act_param_len > 0) { // check whether the ''' character is not in the middle of the word
            VERBOSE(MODULE_EVENT, "%s [ERROR] Bad format of \"%s\" params element - used \'\'\' in the middle of the word.\n", get_formatted_time(), running_modules[module_idx].module_name);
            goto err_cleanup;
         }

         for (y = (x + 1); y < params_len; y++) {
            if (running_modules[module_idx].module_params[y] == '\'') { // parameter in apostrophes MATCH
               if (act_param_len == 0) { // check for empty apostrophes
                  VERBOSE(MODULE_EVENT, "%s [ERROR] Bad format of \"%s\" params element - used empty apostrophes.\n", get_formatted_time(), running_modules[module_idx].module_name);
                  goto err_cleanup;
               }
               x = y;
               goto add_param;
            } else { // add character to parameter in apostrophes
               if (act_param_len >= DEFAULT_SIZE_OF_BUFFER) { // check for reaching maximum length of the parameter
                  VERBOSE(MODULE_EVENT, "%s [ERROR] Too long parameter in \"%s\" params element in apostrophes (> %d).\n", get_formatted_time(), running_modules[module_idx].module_name, DEFAULT_SIZE_OF_BUFFER);
                  goto err_cleanup;
               }
               buffer[act_param_len] = running_modules[module_idx].module_params[y];
               act_param_len++;
            }
         }
         // the terminating ''' was not found
         VERBOSE(MODULE_EVENT, "%s [ERROR] Bad format of \"%s\" params element - used single \'\'\'.\n", get_formatted_time(), running_modules[module_idx].module_name);
         goto err_cleanup;
         break;
      }

      /* parameter in quotes */
      case '\"':
      {
         if (act_param_len > 0) { // check whether the '"' character is not in the middle of the word
            VERBOSE(MODULE_EVENT, "%s [ERROR] Bad format of \"%s\" params element - used \'\"\' in the middle of the word.\n", get_formatted_time(), running_modules[module_idx].module_name);
            goto err_cleanup;
         }

         for (y = (x + 1); y < params_len; y++) {
            if (running_modules[module_idx].module_params[y] == '\"') { // parameter in quotes MATCH
               if (act_param_len == 0) { // check for empty quotes
                  VERBOSE(MODULE_EVENT, "%s [ERROR] Bad format of \"%s\" params element - used empty quotes.\n", get_formatted_time(), running_modules[module_idx].module_name);
                  goto err_cleanup;
               }
               x = y;
               goto add_param;
            } else if (running_modules[module_idx].module_params[y] != '\'') { // add character to parameter in quotes
               if (act_param_len >= DEFAULT_SIZE_OF_BUFFER) { // check for reaching maximum length of the parameter
                  VERBOSE(MODULE_EVENT, "%s [ERROR] Too long parameter in \"%s\" params element in quotes (> %d).\n", get_formatted_time(), running_modules[module_idx].module_name, DEFAULT_SIZE_OF_BUFFER);
                  goto err_cleanup;
               }
               buffer[act_param_len] = running_modules[module_idx].module_params[y];
               act_param_len++;
            } else {
               VERBOSE(MODULE_EVENT, "%s [ERROR] Found apostrophe in \"%s\" params element in quotes.\n", get_formatted_time(), running_modules[module_idx].module_name);
               goto err_cleanup;
            }
         }
         // the terminating '"' was not found
         VERBOSE(MODULE_EVENT, "%s [ERROR] Bad format of \"%s\" params element - used single \'\"\'.\n", get_formatted_time(), running_modules[module_idx].module_name);
         goto err_cleanup;
         break;
      }

      /* parameter delimiter */
      case ' ':
      {
         if (act_param_len == 0) {
            continue; // skip white-spaces between parameters
         }

add_param:
         if (params_cnt == params_arr_size) { // if needed, resize the array of parsed parameters
            params_arr_size += params_arr_size;
            params = (char **) realloc(params, sizeof(char *) * params_arr_size);
            memset(params + ((params_arr_size / 2) * sizeof(char *)), 0, ((params_arr_size / 2) * sizeof(char *)));
         }

         params[params_cnt] = strdup(buffer);
         params_cnt++;
         memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER);
         act_param_len = 0;
         break;
      }

      /* adding one character to parameter out of quotes and apostrophes */
      default:
      {
         if (act_param_len >= DEFAULT_SIZE_OF_BUFFER) { // check for reaching maximum length of the parameter
            VERBOSE(MODULE_EVENT, "%s [WARNING] Too long parameter in \"%s\" params element (> %d)\n", get_formatted_time(), running_modules[module_idx].module_name, DEFAULT_SIZE_OF_BUFFER);
            goto err_cleanup;
         }
         buffer[act_param_len] = running_modules[module_idx].module_params[x];
         act_param_len++;

         if (x == (params_len - 1)) { // if last character of the params element was added, add current module parameter to the params array
            goto add_param;
         }
         break;
      }

      } // end of switch
   }

   if (params_cnt == 0) {
      goto err_cleanup;
   }

   *params_num = params_cnt;
   return params;

err_cleanup:
   for (x = 0; x < params_cnt; x++) {
      NULLP_TEST_AND_FREE(params[x]);
   }
   NULLP_TEST_AND_FREE(params)
   *params_num = 0;
   return NULL;
}

char **prep_module_args(const uint32_t module_idx)
{
   uint32_t x = 0, y = 0, act_dir = 0, ptr = 0;
   uint32_t ifc_spec_size = DEFAULT_SIZE_OF_BUFFER;
   char *ifc_spec = (char *) calloc(ifc_spec_size, sizeof(char));
   char *addr = NULL;
   char *port = NULL;

   char **module_params = NULL;
   uint32_t module_params_num = 0;
   char **bin_args = NULL;
   uint32_t bin_args_num = 2; // initially 2 - at least the name of the future process and terminating NULL pointer
   uint32_t bin_args_pos = 0;

   /* if the module has trap interfaces, one argument for "-i" and one for interfaces specifier */
   if (running_modules[module_idx].module_ifces_cnt > 0) {
      bin_args_num += 2;
   }

   /* if the module has non-empty params, try to parse them */
   if (running_modules[module_idx].module_params != NULL) {
      module_params = parse_module_params(module_idx, &module_params_num);
      if (module_params != NULL && module_params_num > 0) {
         bin_args_num += module_params_num; // after successful params parsing, increment the number of binary arguments
      }
   }

   /* pointers allocation */
   bin_args = (char **) calloc(bin_args_num, sizeof(char *));
   bin_args[0] = strdup(running_modules[module_idx].module_name); // first argument is a name of the future process
   bin_args[bin_args_num - 1] = NULL; // last pointer is NULL because of exec function
   bin_args_pos = 1;

   /* copy already allocated module params strings returned by parse_module_params function */
   if (module_params != NULL && module_params_num > 0) {
      for (x = 0; x < module_params_num; x++) {
         bin_args[bin_args_pos] = module_params[x];
         bin_args_pos++;
      }
      NULLP_TEST_AND_FREE(module_params)
   }

   /* prepare trap interfaces specifier (e.g. "t:1234,u:sock,s:service_sock") */
   if (running_modules[module_idx].module_ifces_cnt > 0) {
      for (y = 0; y < 2; y++) {
         // To get first input ifces and than output ifces
         switch (y) {
         case 0:
            act_dir = IN_MODULE_IFC_DIRECTION;
            break;
         case 1:
            act_dir = OUT_MODULE_IFC_DIRECTION;
            break;
         }

         for (x = 0; x < running_modules[module_idx].module_ifces_cnt; x++) {
            if (running_modules[module_idx].module_ifces[x].int_ifc_direction == act_dir) {
               // Get interface type
               if (running_modules[module_idx].module_ifces[x].int_ifc_type == TCP_MODULE_IFC_TYPE) {
                  strncpy(ifc_spec + ptr, "t:", 2);
                  ptr+=2;
               } else if (running_modules[module_idx].module_ifces[x].int_ifc_type == UNIXSOCKET_MODULE_IFC_TYPE) {
                  strncpy(ifc_spec + ptr, "u:", 2);
                  ptr+=2;
               } else if (running_modules[module_idx].module_ifces[x].int_ifc_type == FILE_MODULE_IFC_TYPE) {
                  strncpy(ifc_spec + ptr, "f:", 2);
                  ptr+=2;
               } else if (running_modules[module_idx].module_ifces[x].int_ifc_type == BLACKHOLE_MODULE_IFC_TYPE) {
                  strncpy(ifc_spec + ptr, "b:", 2);
                  ptr+=2;
               } else {
                  VERBOSE(MODULE_EVENT, "%s [WARNING] Wrong ifc_type in module %d (interface number %d).\n", get_formatted_time(), module_idx, x);
                  NULLP_TEST_AND_FREE(ifc_spec)
                  return NULL;
               }
               // Get interface params
               if (running_modules[module_idx].module_ifces[x].ifc_params != NULL) {
                  if ((strlen(ifc_spec) + strlen(running_modules[module_idx].module_ifces[x].ifc_params) + 1) >= (3 * ifc_spec_size) / 5) {
                     ifc_spec_size += strlen(running_modules[module_idx].module_ifces[x].ifc_params) + (ifc_spec_size / 2);
                     ifc_spec = (char *) realloc(ifc_spec, ifc_spec_size * sizeof(char));
                     memset(ifc_spec + ptr, 0, ifc_spec_size - ptr);
                  }
                  // Compatible with previous format of libtrap -i parameter ("address,port" for one input interface)
                  port = NULL;
                  port = get_param_by_delimiter(running_modules[module_idx].module_ifces[x].ifc_params, &addr, ',');
                  if (port == NULL) {
                     sprintf(ifc_spec + ptr,"%s,",running_modules[module_idx].module_ifces[x].ifc_params);
                  } else {
                     sprintf(ifc_spec + ptr,"%s:%s,", addr, port);
                  }
                  ptr += strlen(running_modules[module_idx].module_ifces[x].ifc_params) + 1;
                  NULLP_TEST_AND_FREE(addr)
               }

            }
         }
      }
      // Remove last comma
      memset(ifc_spec + ptr - 1, 0, 1 * sizeof(char));

      bin_args[bin_args_pos] = strdup(TRAP_PARAM); // add "-i" argument
      bin_args_pos++;
      bin_args[bin_args_pos] = strdup(ifc_spec); // add trap interfaces specifier argument
      bin_args_pos++;
   }

   fprintf(stdout,"%s [INFO] Supervisor - executed command: %s", get_formatted_time(), running_modules[module_idx].module_path);
   fprintf(stderr,"%s [INFO] Supervisor - executed command: %s", get_formatted_time(), running_modules[module_idx].module_path);

   for (x = 1; x < bin_args_num; x++) {
      fprintf(stdout,"   %s",bin_args[x]);
      fprintf(stderr,"   %s",bin_args[x]);
   }

   fprintf(stdout,"\n");
   fprintf(stderr,"\n");

   NULLP_TEST_AND_FREE(ifc_spec)
   return bin_args;
}

int get_number_from_input_choosing_option()
{
   int x = 0;
   int option = 0;
   char *input_p = NULL;
   int input_len = 0;

   input_p = get_input_from_stream(input_fd);
   if (input_p == NULL) {
      goto error_label;
   } else {
      input_len = strlen(input_p);
      // Input must be min 1 and max 3 characters long
      if (input_len > 3 || input_len < 1) {
         goto error_label;
      }
      // Check if all characters are digits
      for (x = 0; x < input_len; x++) {
         if (input_p[x] < '0' || input_p[x] > '9') {
            goto error_label;
         }
      }
      if (sscanf(input_p, "%d", &option) < 1 || option < 0) {
         goto error_label;
      }
   }
   NULLP_TEST_AND_FREE(input_p)
   return option;

error_label:
   NULLP_TEST_AND_FREE(input_p)
   return RET_ERROR;
}

/* Returns count of numbers in input (separated by commas) or -1 */
int parse_numbers_user_selection(int **array)
{
   uint8_t is_num = FALSE;
   uint8_t is_interval = FALSE;
   uint8_t duplicated = FALSE;

   int cur_num = 0;
   int interval_beg = 0;
   int x = 0, y = 0, z = 0;

   int module_nums_cnt = 0;
   int *module_nums = NULL;

   char *input_p = NULL;
   int input_len = 0;

   uint32_t module_nums_size = 10;
   module_nums = (int *) calloc(module_nums_size, sizeof(int));
   input_p = get_input_from_stream(input_fd);


   if (input_p == NULL) {
      goto error_label;
   } else if (strlen(input_p) == 0) {
      VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input - empty string.\n" FORMAT_RESET);
      goto error_label;
   } else {
      input_len = strlen(input_p);
      for (x = 0; x < input_len; x++) {
         if (input_p[x] <= '9' && input_p[x] >= '0') {
            is_num = TRUE;
            cur_num *= 10;
            cur_num += (input_p[x] - '0');
            if ((input_len - 1) > x) {
               continue;
            }
         } else if (input_p[x] == ',') {
            if (x == (strlen(input_p) -1)) {
               VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input - comma at the end.\n" FORMAT_RESET);
               goto error_label;
               break;
            }
            if (is_num == FALSE) {
               VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input - comma without a number before it.\n" FORMAT_RESET);
               goto error_label;
               break;
            }
         } else if (input_p[x] == '-') {
            if (is_num == TRUE && is_interval == FALSE) {
               is_num = FALSE;
               is_interval = TRUE;
               interval_beg = cur_num;
               cur_num = 0;
               continue;
            } else {
               VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input - dash with no number before it.\n" FORMAT_RESET);
               goto error_label;
               break;
            }
         } else {
            VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input - acceptable characters are digits, comma and dash.\n" FORMAT_RESET);
            goto error_label;
            break;
         }

         // Add current number(s)
         if (is_interval == FALSE) {
            interval_beg = cur_num;
         } else if (interval_beg > cur_num) {
            y = interval_beg;
            interval_beg = cur_num;
            cur_num = y;
         }
         for (y = interval_beg; y <= cur_num; y++) {
            duplicated = FALSE;
            // Check whether the current number is already in the array
            for (z = 0; z < module_nums_cnt; z++) {
               if (y == module_nums[z]) {
                  duplicated = TRUE;
                  break;
               }
            }
            if (duplicated == TRUE) {
               continue;
            } else {
               if (module_nums_size == module_nums_cnt) {
                  // reallocate the array with numbers
                  module_nums_size += 20;
                  module_nums = (int *) realloc(module_nums, module_nums_size * sizeof(int));
               }
               module_nums[module_nums_cnt] = y;
               module_nums_cnt++;
            }
         }
         cur_num = 0;
         is_num = FALSE;
         is_interval = FALSE;
      }
   }

   NULLP_TEST_AND_FREE(input_p)
   *array = module_nums;
   return module_nums_cnt;

error_label:
   NULLP_TEST_AND_FREE(module_nums)
   NULLP_TEST_AND_FREE(input_p)
   *array = NULL;
   return RET_ERROR;
}

void init_module_variables(int module_number)
{
   uint x = 0;
   // Allocate needed structures for every modules interface according to its direction or memset if they are already allocated
   for (x = 0; x < running_modules[module_number].module_ifces_cnt; x++) {
      if (running_modules[module_number].module_ifces[x].int_ifc_direction == OUT_MODULE_IFC_DIRECTION) {
         if (running_modules[module_number].module_ifces[x].ifc_data == NULL) {
            running_modules[module_number].module_ifces[x].ifc_data = (void *) calloc(1, sizeof(out_ifc_stats_t));
            running_modules[module_number].module_running = TRUE;
         } else {
            memset(running_modules[module_number].module_ifces[x].ifc_data, 0, sizeof(out_ifc_stats_t));
         }
      } else {
         NULLP_TEST_AND_FREE(running_modules[module_number].module_ifces[x].ifc_data)
      }
   }

   for (x = 0; x < running_modules[module_number].module_ifces_cnt; x++) {
      if (running_modules[module_number].module_ifces[x].int_ifc_direction == IN_MODULE_IFC_DIRECTION) {
         if (running_modules[module_number].module_ifces[x].ifc_data == NULL) {
            running_modules[module_number].module_ifces[x].ifc_data = (void *) calloc(1, sizeof(in_ifc_stats_t));
            running_modules[module_number].module_running = TRUE;
         } else {
            memset(running_modules[module_number].module_ifces[x].ifc_data, 0, sizeof(in_ifc_stats_t));
         }
      }
   }

   // Initialize modules variables
   running_modules[module_number].sent_sigint = FALSE;
   running_modules[module_number].virtual_memory_usage = 0;
   running_modules[module_number].total_cpu_usage_during_module_startup = get_total_cpu_usage();
   running_modules[module_number].last_period_cpu_usage_kernel_mode = 0;
   running_modules[module_number].last_period_cpu_usage_user_mode = 0;
   running_modules[module_number].last_period_percent_cpu_usage_kernel_mode = 0;
   running_modules[module_number].last_period_percent_cpu_usage_user_mode = 0;
   running_modules[module_number].overall_percent_module_cpu_usage_kernel_mode = 0;
   running_modules[module_number].overall_percent_module_cpu_usage_user_mode = 0;
   running_modules[module_number].module_service_sd = -1;
   running_modules[module_number].module_service_ifc_isconnected = FALSE;
   running_modules[module_number].module_service_ifc_conn_attempts = 0;
   running_modules[module_number].module_service_ifc_conn_fails = 0;
   running_modules[module_number].module_service_ifc_conn_block = FALSE;
   running_modules[module_number].module_service_ifc_timer = 0;
}

char *get_param_by_delimiter(const char *source, char **dest, const char delimiter)
{
   char *param_end = NULL;
   unsigned int param_size = 0;

   if (source == NULL) {
      return NULL;
   }

   param_end = strchr(source, delimiter);
   if (param_end == NULL) {
      /* no delimiter found, copy the whole source */
      *dest = strdup(source);
      return NULL;
   }

   param_size = param_end - source;
   *dest = (char *) calloc(1, param_size + 1);
   if (*dest == NULL) {
      return (NULL);
   }
   strncpy(*dest, source, param_size);
   return param_end + 1;
}



void print_statistics()
{
   time_t t = 0;
   time(&t);
   char *stats_buffer = make_formated_statistics((uint8_t) 1);

   if (stats_buffer == NULL) {
      return;
   }
   VERBOSE(STATISTICS, "------> %s", ctime(&t));
   VERBOSE(STATISTICS, "%s", stats_buffer);

   NULLP_TEST_AND_FREE(stats_buffer);
}

void print_statistics_legend()
{
   VERBOSE(STATISTICS,"Legend for an interface statistics:\n"
                        "\tCNT_RM - counter of received messages  on the input interface\n"
                        "\tCNT_RB - counter of received buffers on the input interface\n"
                        "\tCNT_SM - counter of sent messages on the output interface\n"
                        "\tCNT_SB - counter of sent buffers on the output interface\n"
                        "\tCNT_DM - counter of dropped messages on the output interface\n"
                        "\tCNT_AF - autoflush counter of the output interface\n"
                        "Statistics example:\n"
                        "\tmodule_name,interface_direction,interface_number,stats\n"
                        "\tmodule,in,number,CNT_RM,CNT_RB\n"
                        "\tmodule,out,number,CNT_SM,CNT_SB,CNT_DM,CNT_AF\n"
                        "--------------------------------------------------------\n");
}

char *make_formated_statistics(uint8_t stats_mask)
{
   uint8_t print_ifc_stats = FALSE, print_cpu_stats = FALSE, print_memory_stats = FALSE;
   unsigned int size_of_buffer = 5*DEFAULT_SIZE_OF_BUFFER;
   char *buffer = (char *) calloc(size_of_buffer, sizeof(char));
   unsigned int x, y, counter = 0;
   int ptr = 0;

   // Decide which stats should be printed according to the stats mask
   if ((stats_mask & (uint8_t) 1) == (uint8_t) 1) {
      print_ifc_stats = TRUE;
   }
   if ((stats_mask & (uint8_t) 2) == (uint8_t) 2) {
      print_cpu_stats = TRUE;
   }
   if ((stats_mask & (uint8_t) 4) == (uint8_t) 4) {
      print_memory_stats = TRUE;
   }

   if (print_ifc_stats == TRUE) {
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].module_status == TRUE && running_modules[x].module_service_ifc_isconnected == TRUE) {
            counter = 0;
            for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
               if (running_modules[x].module_ifces[y].int_ifc_direction == IN_MODULE_IFC_DIRECTION) {
                  ptr += sprintf(buffer + ptr, "%s,in,%d,%"PRIu64",%"PRIu64"\n", running_modules[x].module_name, counter, ((in_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->recv_msg_cnt,
                                                                                                                                                                                                         ((in_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->recv_buffer_cnt);
                  counter++;
                  if (strlen(buffer) >= (3*size_of_buffer)/5) {
                     size_of_buffer += size_of_buffer/2;
                     buffer = (char *) realloc (buffer, size_of_buffer * sizeof(char));
                     memset(buffer + ptr, 0, size_of_buffer - ptr);
                  }
               }
            }
            counter = 0;
            for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
               if (running_modules[x].module_ifces[y].int_ifc_direction == OUT_MODULE_IFC_DIRECTION) {
                  ptr += sprintf(buffer + ptr, "%s,out,%d,%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n", running_modules[x].module_name, counter, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->sent_msg_cnt,
                                                                                                                                                                                                                              ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->dropped_msg_cnt,
                                                                                                                                                                                                                              ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->sent_buffer_cnt,
                                                                                                                                                                                                                              ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->autoflush_cnt);
                  counter++;
                  if (strlen(buffer) >= (3*size_of_buffer)/5) {
                     size_of_buffer += size_of_buffer/2;
                     buffer = (char *) realloc (buffer, size_of_buffer * sizeof(char));
                     memset(buffer + ptr, 0, size_of_buffer - ptr);
                  }
               }
            }
         }
      }
   }

   if (print_cpu_stats == TRUE) {
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].module_status == TRUE) {
            ptr += sprintf(buffer + ptr, "%s,cpu,%d,%d\n", running_modules[x].module_name, running_modules[x].last_period_percent_cpu_usage_kernel_mode, running_modules[x].last_period_percent_cpu_usage_user_mode);
            if (strlen(buffer) >= (3*size_of_buffer)/5) {
               size_of_buffer += size_of_buffer/2;
               buffer = (char *) realloc (buffer, size_of_buffer * sizeof(char));
               memset(buffer + ptr, 0, size_of_buffer - ptr);
            }
         }
      }
   }

   if (print_memory_stats == TRUE) {
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].module_status == TRUE) {
            ptr += sprintf(buffer + ptr, "%s,mem,%d\n", running_modules[x].module_name, running_modules[x].virtual_memory_usage);
            if (strlen(buffer) >= (3*size_of_buffer)/5) {
               size_of_buffer += size_of_buffer/2;
               buffer = (char *) realloc (buffer, size_of_buffer * sizeof(char));
               memset(buffer + ptr, 0, size_of_buffer - ptr);
            }
         }
      }
   }

   return buffer;
}

int find_loaded_module(char *name)
{
   unsigned int x;
   for (x=0; x<loaded_modules_cnt; x++) {
      if (strcmp(running_modules[x].module_name, name) == 0) {
         return x;
      }
   }
   return -1;
}

void generate_backup_config_file()
{
   FILE *backup_file_fd = NULL;
   char *backup_file_name = NULL;
   modules_profile_t * ptr = first_profile_ptr;
   unsigned int x, y, backuped_modules = 0;
   char buffer[20];
   const char *templ = "<?xml version=\"1.0\"?><nemea-supervisor xmlns=\"urn:cesnet:tmc:nemea:1.0\"></nemea-supervisor>";
   xmlDocPtr document_ptr = NULL;
   xmlNodePtr root_elem = NULL, modules = NULL, module = NULL, trapinterfaces = NULL, interface = NULL;

   document_ptr = xmlParseMemory(templ, strlen(templ));
   if (document_ptr == NULL) {
      return;
   }
   root_elem = xmlDocGetRootElement(document_ptr);
   xmlNewProp (root_elem, BAD_CAST "lock", NULL);
   if (daemon_flag) {
      xmlNewProp (root_elem, BAD_CAST "daemon", BAD_CAST "true");
      xmlNewProp (root_elem, BAD_CAST "socket_path", BAD_CAST socket_path);
   } else {
      xmlNewProp (root_elem, BAD_CAST "daemon", BAD_CAST "false");
      xmlNewProp (root_elem, BAD_CAST "socket_path", BAD_CAST NULL);
   }

   modules = xmlNewChild(root_elem, NULL, BAD_CAST "supervisor", BAD_CAST NULL);
   memset(buffer,0,20);
   sprintf(buffer, "%d", max_restarts_per_minute_config);
   xmlNewChild(modules, NULL, BAD_CAST "module-restarts", BAD_CAST buffer);
   xmlNewChild(modules, NULL, BAD_CAST "logs-directory", BAD_CAST logs_path);

   if (xmlAddChild(root_elem, modules) == NULL) {
      xmlFree(modules);
   }

   // backup modules with profile name
   while (ptr != NULL) {
      if (ptr->profile_name != NULL) {
         modules = xmlNewChild(root_elem, NULL, BAD_CAST "modules", NULL);
         xmlNewChild(modules, NULL, BAD_CAST "name", BAD_CAST ptr->profile_name);
         if (ptr->profile_enabled) {
            xmlNewChild(modules, NULL, BAD_CAST "enabled", BAD_CAST "true");
         } else {
            xmlNewChild(modules, NULL, BAD_CAST "enabled", BAD_CAST "false");
         }
         for (x=0; x<loaded_modules_cnt; x++) {
            if (running_modules[x].modules_profile != NULL) {
               if (strcmp(running_modules[x].modules_profile->profile_name, ptr->profile_name) == 0) {
                  module = xmlNewChild(modules, NULL, BAD_CAST "module", NULL);

                  memset(buffer,0,20);
                  sprintf(buffer, "%d", running_modules[x].module_pid);
                  xmlNewProp (module, BAD_CAST "module_pid", BAD_CAST buffer);

                  if (running_modules[x].module_name != NULL) {
                     xmlNewChild(module, NULL, BAD_CAST "name", BAD_CAST running_modules[x].module_name);
                  }
                  if (running_modules[x].module_path != NULL) {
                     xmlNewChild(module, NULL, BAD_CAST "path", BAD_CAST running_modules[x].module_path);
                  }
                  if (running_modules[x].module_params != NULL) {
                     xmlNewChild(module, NULL, BAD_CAST "params", BAD_CAST running_modules[x].module_params);
                  }
                  if (running_modules[x].module_enabled) {
                     xmlNewChild(module, NULL, BAD_CAST "enabled", BAD_CAST "true");
                  } else {
                     xmlNewChild(module, NULL, BAD_CAST "enabled", BAD_CAST "false");
                  }
                  if (running_modules[x].module_ifces_cnt > 0) {
                     trapinterfaces = xmlNewChild(module, NULL, BAD_CAST "trapinterfaces", NULL);
                  }
                  for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
                     interface = xmlNewChild(trapinterfaces, NULL, BAD_CAST "interface", NULL);
                     if (running_modules[x].module_ifces[y].ifc_note != NULL) {
                        xmlNewChild(interface, NULL, BAD_CAST "note", BAD_CAST running_modules[x].module_ifces[y].ifc_note);
                     }
                     if (running_modules[x].module_ifces[y].ifc_params != NULL) {
                        xmlNewChild(interface, NULL, BAD_CAST "params", BAD_CAST running_modules[x].module_ifces[y].ifc_params);
                     }
                     if (running_modules[x].module_ifces[y].ifc_direction != NULL) {
                        xmlNewChild(interface, NULL, BAD_CAST "direction", BAD_CAST running_modules[x].module_ifces[y].ifc_direction);
                     }
                     if (running_modules[x].module_ifces[y].ifc_type != NULL) {
                        xmlNewChild(interface, NULL, BAD_CAST "type", BAD_CAST running_modules[x].module_ifces[y].ifc_type);
                     }

                     if (xmlAddChild(trapinterfaces, interface) == NULL) {
                        xmlFree(interface);
                     }
                  }


                  if (xmlAddChild(modules, module) == NULL) {
                     xmlFree(module);
                  }
                  backuped_modules++;
               }
            }
         }

         if (xmlAddChild(root_elem, modules) == NULL) {
            xmlFree(modules);
         }
      }
      ptr = ptr->next;
   }

   //backup modules without profile name
   if (backuped_modules < loaded_modules_cnt) {
      modules = xmlNewChild(root_elem, NULL, BAD_CAST "modules", NULL);
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile == NULL) {
            module = xmlNewChild(modules, NULL, BAD_CAST "module", NULL);

            memset(buffer,0,20);
            sprintf(buffer, "%d", running_modules[x].module_pid);
            xmlNewProp (module, BAD_CAST "module_pid", BAD_CAST buffer);

            if (running_modules[x].module_name != NULL) {
               xmlNewChild(module, NULL, BAD_CAST "name", BAD_CAST running_modules[x].module_name);
            }
            if (running_modules[x].module_path != NULL) {
               xmlNewChild(module, NULL, BAD_CAST "path", BAD_CAST running_modules[x].module_path);
            }
            if (running_modules[x].module_params != NULL) {
               xmlNewChild(module, NULL, BAD_CAST "params", BAD_CAST running_modules[x].module_params);
            }
            if (running_modules[x].module_enabled) {
               xmlNewChild(module, NULL, BAD_CAST "enabled", BAD_CAST "true");
            } else {
               xmlNewChild(module, NULL, BAD_CAST "enabled", BAD_CAST "false");
            }
            if (running_modules[x].module_ifces_cnt > 0) {
               trapinterfaces = xmlNewChild(module, NULL, BAD_CAST "trapinterfaces", NULL);
            }
            for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
               interface = xmlNewChild(trapinterfaces, NULL, BAD_CAST "interface", NULL);
               if (running_modules[x].module_ifces[y].ifc_note != NULL) {
                  xmlNewChild(interface, NULL, BAD_CAST "note", BAD_CAST running_modules[x].module_ifces[y].ifc_note);
               }
               if (running_modules[x].module_ifces[y].ifc_params != NULL) {
                  xmlNewChild(interface, NULL, BAD_CAST "params", BAD_CAST running_modules[x].module_ifces[y].ifc_params);
               }
               if (running_modules[x].module_ifces[y].ifc_direction != NULL) {
                  xmlNewChild(interface, NULL, BAD_CAST "direction", BAD_CAST running_modules[x].module_ifces[y].ifc_direction);
               }
               if (running_modules[x].module_ifces[y].ifc_type != NULL) {
                  xmlNewChild(interface, NULL, BAD_CAST "type", BAD_CAST running_modules[x].module_ifces[y].ifc_type);
               }

               if (xmlAddChild(trapinterfaces, interface) == NULL) {
                  xmlFree(interface);
               }
            }

            if (xmlAddChild(modules, module) == NULL) {
               xmlFree(module);
            }
         }
      }

      if (xmlAddChild(root_elem, modules) == NULL) {
         xmlFree(modules);
      }
   }

   backup_file_name = create_backup_file_path();
   if (backup_file_name == NULL) {
      VERBOSE(N_STDOUT, "%s [ERROR] Could not create backup file name!\n", get_formatted_time());
   } else {
      backup_file_fd = fopen(backup_file_name,"w");
      if (backup_file_fd != NULL) {
         if (xmlDocFormatDump(backup_file_fd, document_ptr, 1) == -1) {
            VERBOSE(N_STDOUT, "%s [ERROR] Could not save backup file!\n", get_formatted_time());
         } else {
            VERBOSE(N_STDOUT, "%s [WARNING] Phew, backup file saved !!\n", get_formatted_time());
         }
         fclose(backup_file_fd);
         // Set permissions to backup file to prevent problems during loading and deleting after supervisor restart
         if (chmod(backup_file_name, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == -1) {
            fprintf(stderr, "%s [WARNING] Failed to set permissions to backup file (%s)\n", get_formatted_time(), backup_file_name);
         }
      } else {
         VERBOSE(N_STDOUT, "%s [ERROR] Could not open backup file!\n", get_formatted_time());
      }
      // Create file with information about generated backup file
      create_shutdown_info(&backup_file_name);
      NULLP_TEST_AND_FREE(backup_file_name)
   }

   xmlFreeDoc(document_ptr);
   xmlCleanupParser();
}



/*****************************************************************
 * Functions for getting statistics *
 *****************************************************************/

long int get_total_cpu_usage()
{
   long int new_total_cpu_usage = 0;
   FILE *proc_stat_fd = fopen("/proc/stat","r");
   int x = 0, num = 0;

   if (proc_stat_fd == NULL) {
      return -1;
   } else if (fscanf(proc_stat_fd,"cpu") != 0) {
      fclose(proc_stat_fd);
      return -1;
   }

   for (x=0; x<10; x++) {
      if (!fscanf(proc_stat_fd,"%d",&num)) {
         continue;
      }
      new_total_cpu_usage += num;
   }

   fclose(proc_stat_fd);
   return new_total_cpu_usage;
}

void update_module_cpu_usage()
{
   int utime = 0, stime = 0;
   unsigned int x = 0;
   FILE *proc_stat_fd = NULL;
   char path[20];
   long int new_total_cpu_usage = get_total_cpu_usage();
   long int difference_total = new_total_cpu_usage - last_total_cpu_usage;

   last_total_cpu_usage = new_total_cpu_usage;

   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_status == TRUE) {
         memset(path,0,20*sizeof(char));
         sprintf(path,"/proc/%d/stat",running_modules[x].module_pid);
         proc_stat_fd = fopen(path,"r");
         if (proc_stat_fd == NULL) {
            continue;
         }
         if (!fscanf(proc_stat_fd,"%*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %*[^' '] %d %d", &utime , &stime)) {
            fclose(proc_stat_fd);
            continue;
         }
         if (running_modules[x].total_cpu_usage_during_module_startup != -1) {
            running_modules[x].overall_percent_module_cpu_usage_kernel_mode = 100 * ((float)stime/(float)(new_total_cpu_usage - running_modules[x].total_cpu_usage_during_module_startup));
            running_modules[x].overall_percent_module_cpu_usage_user_mode = 100 * ((float)utime/(float)(new_total_cpu_usage - running_modules[x].total_cpu_usage_during_module_startup));
         } else {
            running_modules[x].overall_percent_module_cpu_usage_kernel_mode = 0;
            running_modules[x].overall_percent_module_cpu_usage_user_mode = 0;
         }
         running_modules[x].last_period_percent_cpu_usage_kernel_mode = 100 * (stime - running_modules[x].last_period_cpu_usage_kernel_mode)/difference_total;
         running_modules[x].last_period_percent_cpu_usage_user_mode = 100 * (utime - running_modules[x].last_period_cpu_usage_user_mode)/difference_total;
         running_modules[x].last_period_cpu_usage_kernel_mode = stime;
         running_modules[x].last_period_cpu_usage_user_mode = utime;
         fclose(proc_stat_fd);
      }
   }
}

void update_module_mem_usage()
{
   unsigned int x = 0;
   FILE *proc_status_fd = NULL;
   char *match = NULL;
   int ret_val = 0;
   char path[20];
   char buffer[1024];

   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_status == TRUE) {
         memset(path,0,20*sizeof(char));
         sprintf(path,"/proc/%d/status",running_modules[x].module_pid);
         proc_status_fd = fopen(path,"r");
         if (proc_status_fd == NULL) {
            continue;
         }

         ret_val = fread(buffer, sizeof(char), 1000, proc_status_fd);
         if (ret_val < 1) {
            fclose(proc_status_fd);
            continue;
         }

         buffer[ret_val] = 0;

         match = strstr(buffer, "VmSize");
         if (match != NULL) {
            if (sscanf(match, "%*[^' ']%*[' ']%d", &(running_modules[x].virtual_memory_usage)) < 1) {
               running_modules[x].virtual_memory_usage = 0;
            }
         }
         fclose(proc_status_fd);
      }
   }
}



/*****************************************************************
 * Daemon mode functions *
 *****************************************************************/

int daemon_init_process()
{
   pid_t process_id = 0;
   pid_t sid = 0;

   process_id = fork();
   if (process_id < 0)  {
      VERBOSE(N_STDOUT,"%s [ERROR] Fork: could not initialize daemon process!\n", get_formatted_time());
      return -1;
   } else if (process_id > 0) {
      NULLP_TEST_AND_FREE(templ_config_file)
      NULLP_TEST_AND_FREE(logs_path)
      free_output_file_strings_and_streams();
      fprintf(stdout, "%s [INFO] PID of daemon process: %d.\n", get_formatted_time(), process_id);
      exit(EXIT_SUCCESS);
   }

   umask(0);
   sid = setsid();
   if (sid < 0) {
      VERBOSE(N_STDOUT,"[ERROR] Setsid: calling process is process group leader!\n");
      return -1;
   }

   return 0;
}

int daemon_init_structures()
{
   unsigned int x = 0;

   server_internals = (server_internals_t *) calloc(1, sizeof(server_internals_t));
   if (server_internals == NULL) {
      VERBOSE(N_STDOUT, "%s [ERROR] Could not allocate dameon_internals, cannot proceed without it!\n", get_formatted_time());
      return -1;
   }
   server_internals->clients = (sup_client_t **) calloc(MAX_NUMBER_SUP_CLIENTS, sizeof(sup_client_t*));
   if (server_internals->clients == NULL) {
      VERBOSE(N_STDOUT, "%s [ERROR] Could not allocate structures for clients, cannot proceed without it!\n", get_formatted_time());
      return -1;
   }
   for (x = 0; x < MAX_NUMBER_SUP_CLIENTS; x++) {
      server_internals->clients[x] = (sup_client_t *) calloc(1, sizeof(sup_client_t));
      if (server_internals->clients[x] != NULL) {
         server_internals->clients[x]->client_sd = -1;
         server_internals->clients[x]->client_input_stream_fd = -1;
      } else {
         VERBOSE(N_STDOUT, "%s [ERROR] Could not allocate structures for clients, cannot proceed without it!\n", get_formatted_time());
         return -1;
      }
   }
   // Initialize daemon's structure mutex
   pthread_mutex_init(&server_internals->lock,NULL);

   return 0;
}

int daemon_init_socket()
{
   union tcpip_socket_addr addr;
   memset(&addr, 0, sizeof(addr));
   addr.unix_addr.sun_family = AF_UNIX;
   snprintf(addr.unix_addr.sun_path, sizeof(addr.unix_addr.sun_path) - 1, "%s", socket_path);

   /* if socket file exists, it could be hard to create new socket and bind */
   unlink(socket_path); /* error when file does not exist is not a problem */
   server_internals->server_sd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (server_internals->server_sd == -1) {
      VERBOSE(N_STDOUT, "%s [ERROR] Could not create daemon socket.\n", get_formatted_time());
      return -1;
   }
   if (fcntl(server_internals->server_sd, F_SETFL, O_NONBLOCK) == -1) {
      VERBOSE(N_STDOUT, "%s [ERROR] Could not set nonblocking mode on daemon socket.\n", get_formatted_time());
      return -1;
   }

   if (bind(server_internals->server_sd, (struct sockaddr *) &addr.unix_addr, sizeof(addr.unix_addr)) != -1) {
      if (chmod(socket_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == -1) {
         VERBOSE(N_STDOUT, "%s [WARNING] Failed to set permissions to socket (%s)\n", get_formatted_time(), socket_path);
      }
   } else {
      VERBOSE(N_STDOUT,"%s [ERROR] Bind: could not bind the daemon socket!\n", get_formatted_time());
      return -1;
   }

   if (listen(server_internals->server_sd, MAX_NUMBER_SUP_CLIENTS) == -1) {
      VERBOSE(N_STDOUT,"%s [ERROR] Listen: could not listen on the daemon socket!\n", get_formatted_time());
      return -1;
   }

   return 0;
}


int daemon_mode_initialization()
{
   fflush(stdout);

   // initialize daemon process
   if (daemon_init_process() != 0) {
      return -1;
   }

   // allocate structures needed by daemon process
   if (daemon_init_structures() != 0) {
      return -1;
   }

   // create socket
   if (daemon_init_socket() != 0) {
      return -1;
   }

   daemon_mode_initialized = TRUE;
   VERBOSE(N_STDOUT, "%s [INFO] Daemon process successfully initialized.\n", get_formatted_time());
   return 0;
}


void daemon_mode_server_routine()
{
   last_total_cpu_usage = get_total_cpu_usage();
   unsigned int x = 0;
   int ret_val = 0, new_client = 0;
   struct sockaddr_storage remoteaddr; // client address
   socklen_t addrlen = sizeof remoteaddr;
   fd_set read_fds;
   struct timeval tv;

   pthread_attr_t clients_thread_attr;
   pthread_attr_init(&clients_thread_attr);
   pthread_attr_setdetachstate(&clients_thread_attr, PTHREAD_CREATE_DETACHED);

   VERBOSE(SUP_LOG, "%s [INFO] Starting server thread.\n", get_formatted_time());
   while (server_internals->daemon_terminated == FALSE) {
      FD_ZERO(&read_fds);
      FD_SET(server_internals->server_sd, &read_fds);

      tv.tv_sec = 1;
      tv.tv_usec = 0;

      ret_val = select(server_internals->server_sd+1, &read_fds, NULL, NULL, &tv);
      if (ret_val == -1) {
         // Select error, return -1 and terminate
         VERBOSE(SUP_LOG, "%s [ERROR] Server thread: select call failed.\n", get_formatted_time());
         return;
      } else if (ret_val != 0) {
         if (FD_ISSET(server_internals->server_sd, &read_fds)) {
            new_client = accept(server_internals->server_sd, (struct sockaddr *)&remoteaddr, &addrlen);
            if (new_client == -1) {
               if (errno == EAGAIN || errno == EWOULDBLOCK) {
                  // Some client wanted to connect but before accepting, he canceled the connection attempt
                  VERBOSE(SUP_LOG, "%s [WARNING] Accept would block error, wait for another client.\n", get_formatted_time());
                  continue;
               } else {
                  VERBOSE(SUP_LOG,"%s [ERROR] Server thread: accept call failed.\n", get_formatted_time());
                  continue;
               }
            } else {
               if (server_internals->clients_cnt < MAX_NUMBER_SUP_CLIENTS) {
                  // Find a free spot in the clients buffer for a new client
                  for (x = 0; x < MAX_NUMBER_SUP_CLIENTS; x++) {
                     if (server_internals->clients[x]->client_sd == -1) {
                        VERBOSE(SUP_LOG,"%s [INFO] New client has connected and will be saved to position %d. (client's ID: %d)\n", get_formatted_time(), x, server_internals->next_client_id);
                        server_internals->clients[x]->client_sd = new_client;
                        server_internals->clients[x]->client_id = server_internals->next_client_id;
                        server_internals->clients[x]->client_connected = TRUE;
                        server_internals->next_client_id++;
                        pthread_mutex_lock(&server_internals->lock);
                        server_internals->clients_cnt++;
                        pthread_mutex_unlock(&server_internals->lock);
                        // Serve the new client
                        if (pthread_create(&server_internals->clients[x]->client_thread_id,  &clients_thread_attr, daemon_serve_client_routine, (void *) (server_internals->clients[x])) != 0) {
                           VERBOSE(SUP_LOG, "%s [ERROR] Could not create client's thread.\n", get_formatted_time());
                           close(server_internals->clients[x]->client_sd);
                           server_internals->clients[x]->client_sd = -1;
                           server_internals->clients[x]->client_connected = FALSE;
                           pthread_mutex_lock(&server_internals->lock);
                           server_internals->clients_cnt--;
                           pthread_mutex_unlock(&server_internals->lock);
                        }
                        break;
                     }
                  }
               } else {
                  // Daemon cannot accept another client -> reject the new client
                  VERBOSE(SUP_LOG, "[WARNING] New client has connected, but there is too many clients - cannot accept another one.\n");
                  close(new_client);
                  continue;
               }
            }
         }
      } else {
         /* Select timeout - nothing to do (waiting for incoming connections). */
      }
   }
   return;
}


int daemon_get_code_from_client(sup_client_t **cli)
{
   sup_client_t * client = *cli;
   int bytes_to_read = 0; // value can be also -1 <=> ioctl error
   int ret_val = 0;
   int request = -1;
   char *buffer = NULL;
   fd_set read_fds;
   struct timeval tv;

   while (1) {
         FD_ZERO(&read_fds);
         FD_SET(client->client_input_stream_fd, &read_fds);

         tv.tv_sec = 2;
         tv.tv_usec = 0;

         ret_val = select(client->client_input_stream_fd+1, &read_fds, NULL, NULL, &tv);
         if (ret_val == -1) {
            // select error, return -1 and wait for new client
            return -1;
         } else if (ret_val != 0) {
            if (FD_ISSET(client->client_input_stream_fd, &read_fds)) {
               ioctl(client->client_input_stream_fd, FIONREAD, &bytes_to_read);
               if (bytes_to_read == 0 || bytes_to_read == -1) {
                  // client has disconnected, return -2 and wait for new client
                  return -2;
               }

               buffer = get_input_from_stream(client->client_input_stream);
               if (buffer == NULL) {
                  // problem with input, return -1 and wait for new client
                  return -1;
               }
               if (sscanf(buffer, "%d", &request) != 1) {
                  // wrong format of code, return -1 and wait for new client
                  free(buffer);
                  return -1;
               }
               free(buffer);

               switch (request) {
               case CLIENT_CONFIG_MODE_CODE:
                  return CLIENT_CONFIG_MODE_CODE;

               case CLIENT_RELOAD_MODE_CODE:
                  return CLIENT_RELOAD_MODE_CODE;

               case CLIENT_STATS_MODE_CODE:
                  return CLIENT_STATS_MODE_CODE;

               default:
                  // unknown code, return -1 and wait for new client
                  return -1;
               }
            }
         } else {
            // timeout instead of mode-code, return -3 and wait for new client
            return -3;
         }
      }
}

void daemon_send_options_to_client()
{
   usleep(50000); // Solved bugged output - without this sleep, escape codes in output were not sometimes reseted on time and they were applied also on this menu
   VERBOSE(N_STDOUT, FORMAT_MENU FORMAT_BOLD "--------OPTIONS--------\n" FORMAT_RESET);
   VERBOSE(N_STDOUT, FORMAT_MENU "1. ENABLE ALL MODULES\n");
   VERBOSE(N_STDOUT, "2. DISABLE ALL MODULES\n");
   VERBOSE(N_STDOUT, "3. ENABLE MODULE OR PROFILE\n");
   VERBOSE(N_STDOUT, "4. DISABLE MODULE OR PROFILE\n");
   VERBOSE(N_STDOUT, "5. CONFIGURATION STATUS\n");
   VERBOSE(N_STDOUT, "6. AVAILABLE MODULES\n");
   VERBOSE(N_STDOUT, "7. RELOAD CONFIGURATION\n");
   VERBOSE(N_STDOUT, "8. PRINT SUPERVISOR INFO\n");
   VERBOSE(N_STDOUT, "9. SHOW LOGS\n");
   VERBOSE(N_STDOUT, "-- Type \"Cquit\" to exit client --\n");
   VERBOSE(N_STDOUT, "-- Type \"Dstop\" to stop daemon --\n" FORMAT_RESET);
   VERBOSE(N_STDOUT, FORMAT_INTERACTIVE "[INTERACTIVE] Your choice: " FORMAT_RESET);
}

int daemon_open_client_streams(sup_client_t **cli)
{
   // open input stream on client' s socket
   sup_client_t * client = *cli;
   client->client_input_stream = fdopen(client->client_sd, "r");
   if (client->client_input_stream == NULL) {
      VERBOSE(N_STDOUT,"%s [ERROR] Fdopen: could not open client's input stream! (client's ID: %d)\n", get_formatted_time(), client->client_id);
      return -1;
   }

   // open output stream on client' s socket
   client->client_output_stream = fdopen(client->client_sd, "w");
   if (client->client_output_stream == NULL) {
      VERBOSE(N_STDOUT,"%s [ERROR] Fdopen: could not open client's output stream! (client's ID: %d)\n", get_formatted_time(), client->client_id);
      return -1;
   }

   // get file descriptor of input stream on client' s socket
   client->client_input_stream_fd = fileno(client->client_input_stream);
   if (client->client_input_stream_fd < 0) {
      VERBOSE(N_STDOUT,"%s [ERROR] Fileno: could not get client's input stream descriptor! (client's ID: %d)\n", get_formatted_time(), client->client_id);
      return -1;
   }

   return 0;
}

void daemon_disconnect_client(sup_client_t *cli)
{
   cli->client_connected = FALSE;
   if (cli->client_input_stream_fd >= 0) {
      close(cli->client_input_stream_fd);
      cli->client_input_stream_fd = -1;
   }
   if (cli->client_input_stream != NULL) {
      fclose(cli->client_input_stream);
      cli->client_input_stream = NULL;
   }
   if (cli->client_output_stream != NULL) {
      fclose(cli->client_output_stream);
      cli->client_output_stream = NULL;
   }
   if (cli->client_sd >= 0) {
      close(cli->client_sd);
      cli->client_sd = -1;
   }
   pthread_mutex_lock(&server_internals->lock);
   server_internals->clients_cnt--;
   pthread_mutex_unlock(&server_internals->lock);
   VERBOSE(SUP_LOG, "%s [INFO] Disconnected client. (client's ID: %d)\n", get_formatted_time(), cli->client_id);
}

void *daemon_serve_client_routine (void *cli)
{
   sup_client_t * client = (sup_client_t *) cli;
   int bytes_to_read = 0; // value can be also -1 <=> ioctl error
   int ret_val = 0, nine_cnt = 0;
   int request = -1;
   fd_set read_fds;
   struct timeval tv;

   // Open client's streams
   if (daemon_open_client_streams(&client) != 0) {
      daemon_disconnect_client(client);
      pthread_exit(EXIT_SUCCESS);
   }

   // get code from client according to operation he wants to perform
   switch (daemon_get_code_from_client(&client)) {
   case -3: // timeout
      VERBOSE(SUP_LOG, "[ERROR] Timeout, client has not sent mode-code -> gonna wait for new client\n");
      daemon_disconnect_client(client);
      pthread_exit(EXIT_SUCCESS);

   case -2: // client has disconnected
      VERBOSE(SUP_LOG, "[ERROR] Client has disconnected -> gonna wait for new client\n");
      daemon_disconnect_client(client);
      pthread_exit(EXIT_SUCCESS);

   case -1: // another error while receiving mode-code from client
      VERBOSE(SUP_LOG, "[ERROR] Error while waiting for a mode-code from client -> gonna wait for new client\n");
      daemon_disconnect_client(client);
      pthread_exit(EXIT_SUCCESS);

   case CLIENT_CONFIG_MODE_CODE: // normal client configure mode -> continue to options loop
      // Check whether any client is already connected in config mode
      pthread_mutex_lock(&server_internals->lock);
      if (server_internals->config_mode_active == TRUE) {
         VERBOSE(SUP_LOG, "%s [INFO] Got configuration mode code, but another client is already connected in this mode. (client's ID: %d)\n", get_formatted_time(), client->client_id);
         fprintf(client->client_output_stream, FORMAT_WARNING "[WARNING] Another client is connected to supervisor in configuration mode, you have to wait.\n" FORMAT_RESET);
         fflush(client->client_output_stream);
         pthread_mutex_unlock(&server_internals->lock);
         daemon_disconnect_client(client);
         pthread_exit(EXIT_SUCCESS);
      } else {
         VERBOSE(SUP_LOG, "%s [INFO] Got configuration mode code. (client's ID: %d)\n", get_formatted_time(), client->client_id);
         server_internals->config_mode_active = TRUE;
      }
      pthread_mutex_unlock(&server_internals->lock);
      output_fd = client->client_output_stream;
      input_fd = client->client_input_stream;
      daemon_send_options_to_client();
      break;

   case CLIENT_RELOAD_MODE_CODE: // just reload configuration and wait for new client
      VERBOSE(SUP_LOG, "%s [INFO] Got reload mode code. (client's ID: %d)\n", get_formatted_time(), client->client_id);
      daemon_disconnect_client(client);
      reload_configuration(RELOAD_DEFAULT_CONFIG_FILE, NULL);
      pthread_exit(EXIT_SUCCESS);

   case CLIENT_STATS_MODE_CODE: { // send stats to current client and wait for new one
      VERBOSE(SUP_LOG, "%s [INFO] Got stats mode code. (client's ID: %d)\n", get_formatted_time(), client->client_id);
      update_module_cpu_usage();
      update_module_mem_usage();
      char *stats_buffer = make_formated_statistics((uint8_t) 7);
      int buffer_len = strlen(stats_buffer);
      char stats_buffer2[buffer_len+1];
      memset(stats_buffer2,0,buffer_len+1);
      strncpy(stats_buffer2, stats_buffer, buffer_len+1);
      fprintf(client->client_output_stream, "%s", stats_buffer2);
      fflush(client->client_output_stream);
      NULLP_TEST_AND_FREE(stats_buffer)
      VERBOSE(SUP_LOG, "%s [INFO] Stats sent to client. (client's ID: %d)\n", get_formatted_time(), client->client_id);
      daemon_disconnect_client(client);
      pthread_exit(EXIT_SUCCESS);
   }

   default: // just in case of unknown return value.. clean up and wait for new client
      daemon_disconnect_client(client);
      pthread_exit(EXIT_SUCCESS);
   }

   // Configuration mode MAIN LOOP
   while (client->client_connected == TRUE && server_internals->daemon_terminated == FALSE) {
      request = -1;
      FD_ZERO(&read_fds);
      FD_SET(client->client_input_stream_fd, &read_fds);

      tv.tv_sec = 0;
      tv.tv_usec = 500000;

      ret_val = select(client->client_input_stream_fd+1, &read_fds, NULL, NULL, &tv);
      if (ret_val == -1) {
         VERBOSE(SUP_LOG,"%s [ERROR] Client's thread: select error.\n", get_formatted_time());
         input_fd = stdin;
         output_fd = supervisor_log_fd;
         pthread_mutex_lock(&server_internals->lock);
         server_internals->config_mode_active = FALSE;
         pthread_mutex_unlock(&server_internals->lock);
         daemon_disconnect_client(client);
         pthread_exit(EXIT_SUCCESS);
      } else if (ret_val != 0) {
         if (FD_ISSET(client->client_input_stream_fd, &read_fds)) {
            ioctl(client->client_input_stream_fd, FIONREAD, &bytes_to_read);
            if (bytes_to_read == 0 || bytes_to_read == -1) {
               input_fd = stdin;
               output_fd = supervisor_log_fd;
               pthread_mutex_lock(&server_internals->lock);
               server_internals->config_mode_active = FALSE;
               pthread_mutex_unlock(&server_internals->lock);
               daemon_disconnect_client(client);
               pthread_exit(EXIT_SUCCESS);
            }

            request = get_number_from_input_choosing_option();

            switch (request) {
            case 1:
               interactive_start_configuration();
               break;
            case 2:
               interactive_stop_configuration();
               break;
            case 3:
               interactive_set_enabled();
               break;
            case 4:
               interactive_set_disabled();
               break;
            case 5:
               interactive_show_running_modules_status();
               break;
            case 6:
               interactive_show_available_modules();
               break;
            case 7:
               reload_configuration(RELOAD_DEFAULT_CONFIG_FILE, NULL);
               break;
            case 8:
               interactive_print_supervisor_info();
               break;
            case 9:
               interactive_show_logs();
               break;
            case 0:
               nine_cnt++;
               if (nine_cnt == 3) {
                  pthread_mutex_lock(&server_internals->lock);
                  server_internals->daemon_terminated = TRUE;
                  pthread_mutex_unlock(&server_internals->lock);
               }
               break;
            default:
               VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input!\n" FORMAT_RESET);
               break;
            }
            if (nine_cnt == 0 && !(server_internals->daemon_terminated) && client->client_connected) {
               daemon_send_options_to_client();
            }
         }
      } else {
         if (nine_cnt > 0) {
            VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input!\n" FORMAT_RESET);
            nine_cnt = 0;
            daemon_send_options_to_client();
         }
      }
   }

   input_fd = stdin;
   output_fd = supervisor_log_fd;
   pthread_mutex_lock(&server_internals->lock);
   server_internals->config_mode_active = FALSE;
   pthread_mutex_unlock(&server_internals->lock);
   daemon_disconnect_client(client);
   pthread_exit(EXIT_SUCCESS);
}



/*****************************************************************
 * Service thread functions *
 *****************************************************************/

void service_start_module(const int module_idx)
{
   uint x = 0;

   if (running_modules[module_idx].module_running == FALSE) {
      VERBOSE(MODULE_EVENT,"%s [START] Starting module %s.\n", get_formatted_time(), running_modules[module_idx].module_name);
      #ifdef nemea_plugin
         netconf_notify(MODULE_EVENT_STARTED,running_modules[module_idx].module_name);
      #endif
      // In case that reloading configuration changes module (its interfaces), module_running is set to FALSE and interfaces data are freed
      for (x = 0; x < running_modules[module_idx].module_ifces_cnt; x++) {
         NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces[x].ifc_data)
      }
      running_modules[module_idx].module_running = TRUE;
   } else {
      #ifdef nemea_plugin
         netconf_notify(MODULE_EVENT_RESTARTED,running_modules[module_idx].module_name);
      #endif
      VERBOSE(MODULE_EVENT,"%s [RESTART] Restarting module %s\n", get_formatted_time(), running_modules[module_idx].module_name);
   }

   char log_path_stdout[PATH_MAX];
   char log_path_stderr[PATH_MAX];
   memset(log_path_stderr, 0, PATH_MAX);
   memset(log_path_stdout, 0, PATH_MAX);

   if (modules_logs_path_initialized == TRUE) {
      sprintf(log_path_stdout,"%smodules_logs/%s_stdout",logs_path, running_modules[module_idx].module_name);
      sprintf(log_path_stderr,"%smodules_logs/%s_stderr",logs_path, running_modules[module_idx].module_name);
   }

   init_module_variables(module_idx);

   time_t rawtime;
   struct tm * timeinfo;
   time ( &rawtime );
   timeinfo = localtime ( &rawtime );

   fflush(stdout);
   running_modules[module_idx].module_pid = fork();
   if (running_modules[module_idx].module_pid == 0) {
      int fd_stdout = open(log_path_stdout, O_RDWR | O_CREAT | O_APPEND, PERM_LOGFILE);
      int fd_stderr = open(log_path_stderr, O_RDWR | O_CREAT | O_APPEND, PERM_LOGFILE);
      if (fd_stdout != -1) {
         dup2(fd_stdout,1); //stdout
         close(fd_stdout);
      }
      if (fd_stderr != -1) {
         dup2(fd_stderr,2); //stderr
         close(fd_stderr);
      }
      setsid(); // important for sending SIGINT to supervisor.. modules can't receive the signal too !!!
      fprintf(stdout,"---> %s", asctime (timeinfo));
      fprintf(stderr,"---> %s", asctime (timeinfo));
      if (running_modules[module_idx].module_path == NULL) {
         VERBOSE(N_STDOUT,"%s [ERROR] Starting module: module path is missing!\n", get_formatted_time());
         running_modules[module_idx].module_enabled = FALSE;
      } else {
         char **params = prep_module_args(module_idx);
         if (params == NULL) {
            goto execute_fail;
         }
         fflush(stdout);
         fflush(stderr);
         execvp(running_modules[module_idx].module_path, params);
execute_fail:
         exit(EXIT_FAILURE);
      }
      VERBOSE(MODULE_EVENT,"%s [ERROR] Module execution: could not execute %s binary! (possible reason - wrong module binary path)\n", get_formatted_time(), running_modules[module_idx].module_name);
      running_modules[module_idx].module_enabled = FALSE;
      exit(EXIT_FAILURE);
   } else if (running_modules[module_idx].module_pid == -1) {
      running_modules[module_idx].module_status = FALSE;
      running_modules[module_idx].module_restart_cnt++;
      VERBOSE(N_STDOUT,"%s [ERROR] Fork: could not fork supervisor process!\n", get_formatted_time());
   } else {
      running_modules[module_idx].module_is_my_child = TRUE;
      running_modules[module_idx].module_status = TRUE;
      running_modules[module_idx].module_restart_cnt++;
      if (running_modules[module_idx].module_restart_cnt == 1) {
         running_modules[module_idx].module_restart_timer = 0;
      }
   }
}

void service_disconnect_from_module(const int module_idx)
{
   if (running_modules[module_idx].module_service_ifc_isconnected == TRUE) {
      VERBOSE(MODULE_EVENT,"%s [SERVICE] Disconnecting from module %s\n", get_formatted_time(), running_modules[module_idx].module_name);
      if (running_modules[module_idx].module_service_sd != -1) {
         close(running_modules[module_idx].module_service_sd);
         running_modules[module_idx].module_service_sd = -1;
      }
      running_modules[module_idx].module_service_ifc_isconnected = FALSE;
   }

   running_modules[module_idx].module_service_ifc_conn_fails++;

   if (running_modules[module_idx].module_service_ifc_conn_fails == 1) {
      running_modules[module_idx].module_service_ifc_timer = 0;
   }
}

// Returns a number of running modules
int service_check_modules_status()
{
   unsigned int x, some_module_running = 0;

   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_pid > 0) {
         if (kill(running_modules[x].module_pid, 0) == -1) {
            if (errno == EINVAL) {
               VERBOSE(MODULE_EVENT,"%s [STOP] kill -0: ernno EINVAL\n", get_formatted_time());
            }
            if (errno == EPERM) {
               VERBOSE(MODULE_EVENT,"%s [STOP] kill -0: errno EPERM\n", get_formatted_time());
            }
            if (errno == ESRCH) {
               VERBOSE(MODULE_EVENT,"%s [STOP] kill -0: module %s (PID: %d) is not running !\n", get_formatted_time(), running_modules[x].module_name, running_modules[x].module_pid);
            }
            if (running_modules[x].module_service_sd != -1) {
                  close(running_modules[x].module_service_sd);
                  running_modules[x].module_service_sd = -1;
            }
            running_modules[x].module_status = FALSE;
            running_modules[x].module_service_ifc_isconnected = FALSE;
            running_modules[x].module_pid = 0;
         } else {
            running_modules[x].module_status = TRUE;
            some_module_running++;
         }
      }
   }
   return some_module_running;
}

void service_clean_after_children()
{
   pid_t result;
   unsigned int x;
   int status;

   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_pid > 0 && running_modules[x].module_is_my_child) {
         result = waitpid(running_modules[x].module_pid , &status, WNOHANG);
         if (result == 0) {
           // Child still alive, nothing to do here!
         } else if (result == -1) {
           // Error
            if (errno == ECHILD) {
               VERBOSE(MODULE_EVENT, "%s [CLEAN] waitpid: module %s (PID: %d) is not my child!\n", get_formatted_time(), running_modules[x].module_name, running_modules[x].module_pid);
               running_modules[x].module_is_my_child = FALSE;
            }
         } else {
           // Child exited
            VERBOSE(MODULE_EVENT, "%s [CLEAN] waitpid: module %s (PID: %d) is my child and is not alive anymore!\n", get_formatted_time(), running_modules[x].module_name, running_modules[x].module_pid);
         }
      }
   }
}

void service_stop_modules_sigint()
{
   unsigned int x;
   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_status == TRUE && (running_modules[x].modules_profile->profile_enabled == FALSE || running_modules[x].module_enabled == FALSE) && running_modules[x].sent_sigint == FALSE) {
         #ifdef nemea_plugin
            netconf_notify(MODULE_EVENT_STOPPED,running_modules[x].module_name);
         #endif
         VERBOSE(MODULE_EVENT, "%s [STOP] Stopping module %s... sending SIGINT\n", get_formatted_time(), running_modules[x].module_name);
         kill(running_modules[x].module_pid,2);
         running_modules[x].sent_sigint = TRUE;
      }
   }
}

void service_stop_modules_sigkill()
{
   // service_sock_spec size is length of "service_PID" where PID is max 5 chars (8 + 5 + 1 zero terminating)
   char service_sock_spec[14];
   char *dest_port = NULL;
   char buffer[DEFAULT_SIZE_OF_BUFFER];
   unsigned int x, y;

   for (x = 0; x < loaded_modules_cnt; x++) {
      if (running_modules[x].module_status == TRUE && (running_modules[x].module_enabled == FALSE || running_modules[x].modules_profile->profile_enabled == FALSE) && running_modules[x].sent_sigint == TRUE) {
         VERBOSE(MODULE_EVENT, "%s [STOP] Stopping module %s... sending SIGKILL\n", get_formatted_time(), running_modules[x].module_name);
         kill(running_modules[x].module_pid,9);

         // Delete all unix-socket files after killing the module
         for (y = 0; y < running_modules[x].module_ifces_cnt; y++) {
            // Delete unix-socket created by modules output interface
            if ((running_modules[x].module_ifces[y].int_ifc_type == UNIXSOCKET_MODULE_IFC_TYPE) && (running_modules[x].module_ifces[y].int_ifc_direction == OUT_MODULE_IFC_DIRECTION)) {
               if (running_modules[x].module_ifces[y].ifc_params == NULL) {
                  continue;
               }
               memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER);
               get_param_by_delimiter(running_modules[x].module_ifces[y].ifc_params, &dest_port, ',');
               sprintf(buffer,MODULES_UNIXSOCKET_PATH_FILENAME_FORMAT,dest_port);
               VERBOSE(MODULE_EVENT, "%s [CLEAN] Deleting socket %s - module %s\n", get_formatted_time(), buffer, running_modules[x].module_name);
               unlink(buffer);
               NULLP_TEST_AND_FREE(dest_port)
            }
         }

         // Delete unix-socket created by modules service interface
         if (running_modules[x].module_ifces_cnt > 0) {
            memset(service_sock_spec, 0, 14 * sizeof(char));
            sprintf(service_sock_spec, "service_%d", running_modules[x].module_pid);
            sprintf(buffer, MODULES_UNIXSOCKET_PATH_FILENAME_FORMAT, service_sock_spec);
            VERBOSE(MODULE_EVENT, "%s [CLEAN] Deleting socket %s - module %s\n", get_formatted_time(), buffer, running_modules[x].module_name);
            unlink(buffer);
         }
      }
   }
}



void service_update_modules_status()
{
   unsigned int x = 0;
   int max_restarts = 0;

   for (x=0; x<loaded_modules_cnt; x++) {
      if (++running_modules[x].module_restart_timer >= NUM_SERVICE_IFC_PERIODS) {
         running_modules[x].module_restart_timer = 0;
         running_modules[x].module_restart_cnt = 0;
      }

      // TODO why assigning this value in every service thread cycle???
      if (running_modules[x].module_max_restarts_per_minute > -1) {
         max_restarts = running_modules[x].module_max_restarts_per_minute;
      } else {
         max_restarts = max_restarts_per_minute_config;
      }

      if (running_modules[x].modules_profile->profile_enabled == TRUE && running_modules[x].module_enabled == TRUE && running_modules[x].module_status == FALSE && (running_modules[x].module_restart_cnt == max_restarts)) {
         VERBOSE(MODULE_EVENT,"%s [RESTART] Module: %s was restarted %d times per minute and it is down again. I set it disabled.\n", get_formatted_time(), running_modules[x].module_name, max_restarts);
         running_modules[x].module_enabled = FALSE;
         #ifdef nemea_plugin
            netconf_notify(MODULE_EVENT_DISABLED,running_modules[x].module_name);
         #endif
      } else if (running_modules[x].modules_profile->profile_enabled == TRUE && running_modules[x].module_status == FALSE && running_modules[x].module_enabled == TRUE) {
         service_start_module(x);
      }
   }
}


void service_check_connections()
{
   uint x = 0;

    for (x = 0; x < loaded_modules_cnt; x++) {
         // If supervisor couldn't connect to service interface or too many errors during sending/receiving occurred, connecting is blocked
         if (running_modules[x].module_service_ifc_conn_block == TRUE) {
            continue;
         }

         // Check whether the module has service interface and is running
         if (running_modules[x].module_status == TRUE) {

            if (++running_modules[x].module_service_ifc_timer >= NUM_SERVICE_IFC_PERIODS) {
               running_modules[x].module_service_ifc_timer = 0;
               running_modules[x].module_service_ifc_conn_fails = 0;
            }

            if (running_modules[x].module_service_ifc_conn_fails >= MAX_SERVICE_IFC_CONN_FAILS) {
               VERBOSE(MODULE_EVENT, "%s [WARNING] Module %s reached %d errors during connections -> it is blocked.\n", get_formatted_time(), running_modules[x].module_name, MAX_SERVICE_IFC_CONN_FAILS);
               running_modules[x].module_service_ifc_conn_block = TRUE;
               continue;
            }

            // Check connection between module and supervisor, if they are not connected and number of attempts <= 3, try to connect
            if (running_modules[x].module_service_ifc_isconnected == FALSE) {
               // Check module socket descriptor, closed socket has descriptor set to -1
               if (running_modules[x].module_service_sd != -1) {
                  close(running_modules[x].module_service_sd);
                  running_modules[x].module_service_sd = -1;
               }
               service_connect_to_module(x);
            }
         }
      }
}


int service_recv_data(int module_idx, uint32_t size, void **data)
{
   int num_of_timeouts = 0;
   int total_receved = 0;
   int last_receved = 0;

   while (total_receved < size) {
      last_receved = recv(running_modules[module_idx].module_service_sd, (*data) + total_receved, size - total_receved, MSG_DONTWAIT);
      if (last_receved == 0) {
         VERBOSE(STATISTICS,"! Modules service thread closed its socket, im done !\n");
         return -1;
      } else if (last_receved == -1) {
         if (errno == EAGAIN  || errno == EWOULDBLOCK) {
            num_of_timeouts++;
            if (num_of_timeouts >= 3) {
               return -1;
            } else {
               usleep(SERVICE_WAIT_BEFORE_TIMEOUT);
               continue;
            }
         }
         VERBOSE(MODULE_EVENT,"%s [SERVICE] Error while receiving from module %d_%s !\n", get_formatted_time(), module_idx, running_modules[module_idx].module_name);
         return -1;
      }
      total_receved += last_receved;
   }
   return 0;
}

int service_send_data(int module_idx, uint32_t size, void **data)
{
   int num_of_timeouts = 0, total_sent = 0, last_sent = 0;

   while (total_sent < size) {
      last_sent = send(running_modules[module_idx].module_service_sd, (*data) + total_sent, size - total_sent, MSG_DONTWAIT);
      if (last_sent == -1) {
         if (errno == EAGAIN  || errno == EWOULDBLOCK) {
            num_of_timeouts++;
            if (num_of_timeouts >= 3) {
               return -1;
            } else {
               usleep(SERVICE_WAIT_BEFORE_TIMEOUT);
               continue;
            }
         }
         VERBOSE(MODULE_EVENT,"%s [SERVICE] Error while sending to module %d_%s !\n", get_formatted_time(), module_idx, running_modules[module_idx].module_name);
         return -1;
      }
      total_sent += last_sent;
   }
   return 0;
}

void service_connect_to_module(const int module)
{
   // service_sock_spec size is length of "service_PID" where PID is max 5 chars (8 + 5 + 1 zero terminating)
   char service_sock_spec[14];
   int sockfd = -1;
   union tcpip_socket_addr addr;

   // Increase counter of connection attempts to the service interface
   running_modules[module].module_service_ifc_conn_attempts++;

   if (running_modules[module].module_service_ifc_conn_attempts > SERVICE_IFC_CONN_ATTEMPTS_LIMIT) {
      VERBOSE(MODULE_EVENT,"%s [WARNING] Connection attempts to service interface of module %s exceeded %d, enough trying!\n", get_formatted_time(), running_modules[module].module_name, SERVICE_IFC_CONN_ATTEMPTS_LIMIT);
      running_modules[module].module_service_ifc_conn_block = TRUE;
      return;
   }

   memset(service_sock_spec, 0, 14 * sizeof(char));
   sprintf(service_sock_spec, "service_%d", running_modules[module].module_pid);
   VERBOSE(MODULE_EVENT,"%s [SERVICE] Connecting to module %s on port %s...\n", get_formatted_time(), running_modules[module].module_name, service_sock_spec);

   memset(&addr, 0, sizeof(addr));

   addr.unix_addr.sun_family = AF_UNIX;
   snprintf(addr.unix_addr.sun_path, sizeof(addr.unix_addr.sun_path) - 1, MODULES_UNIXSOCKET_PATH_FILENAME_FORMAT, service_sock_spec);
   sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (sockfd == -1) {
      VERBOSE(MODULE_EVENT,"%s [SERVICE] Error while opening socket for connection with module %s.\n", get_formatted_time(), running_modules[module].module_name);
      running_modules[module].module_service_ifc_isconnected = FALSE;
      return;
   }
   if (connect(sockfd, (struct sockaddr *) &addr.unix_addr, sizeof(addr.unix_addr)) == -1) {
      VERBOSE(MODULE_EVENT,"%s [SERVICE] Error while connecting to module %s on port %s\n", get_formatted_time(), running_modules[module].module_name, service_sock_spec);
      running_modules[module].module_service_ifc_isconnected = FALSE;
      close(sockfd);
      return;
   }
   running_modules[module].module_service_sd = sockfd;
   running_modules[module].module_service_ifc_isconnected = TRUE;
   VERBOSE(MODULE_EVENT,"%s [SERVICE] Connected to module %s.\n", get_formatted_time(), running_modules[module].module_name);
}

void *service_thread_routine(void *arg __attribute__ ((unused)))
{
   uint64_t period_cnt = 0;
   service_msg_header_t *header = (service_msg_header_t *) calloc(1, sizeof(service_msg_header_t));
   uint32_t buffer_size = 256;
   char *buffer = (char *) calloc(buffer_size, sizeof(char));
   int running_modules_cnt = 0;
   unsigned int x,y;

   while (TRUE) {
      pthread_mutex_lock(&running_modules_lock);

      running_modules_cnt = service_check_modules_status();
      if (service_thread_continue == FALSE) {
         if (service_stop_all_modules == FALSE) {
            VERBOSE(N_STDOUT, "%s [WARNING] I let modules continue running!\n", get_formatted_time());
            break;
         } else if (running_modules_cnt == 0) {
            VERBOSE(N_STDOUT, "%s [WARNING] I stopped all modules!\n", get_formatted_time());
            break;
         }
      }
      service_update_modules_status();
      service_stop_modules_sigint();

      usleep(SERVICE_WAIT_FOR_MODULES_TO_FINISH);

      service_clean_after_children();
      running_modules_cnt = service_check_modules_status();
      service_stop_modules_sigkill();
      service_clean_after_children();

      for (y=0; y<loaded_modules_cnt; y++) {
         if (running_modules[y].module_served_by_service_thread == FALSE) {
            if (running_modules[y].remove_module == TRUE) {
               if (running_modules[y].module_status == FALSE) {
                  free_module_and_shift_array(y);
               }
            } else if (running_modules[y].init_module == TRUE) {
               if (running_modules[y].module_status == FALSE) {
                  running_modules[y].module_enabled = TRUE;
                  running_modules[y].module_restart_cnt = -1;
                  running_modules[y].init_module = FALSE;
                  running_modules[y].module_served_by_service_thread = TRUE;
               } else {
                  service_disconnect_from_module(y);
               }
            } else {
               running_modules[y].module_served_by_service_thread = TRUE;
            }
         }
      }

      // Update status of every module before sending a request for their stats
      running_modules_cnt = service_check_modules_status();

      // Set request header
      header->com = SERVICE_GET_COM;
      header->data_size = 0;

      // Handle connection between supervisor and modules via service interface
      service_check_connections();

      for (x=0;x<loaded_modules_cnt;x++) {
         // If the module and supervisor are connected via service interface, request for stats is sent
         if (running_modules[x].module_service_ifc_isconnected == TRUE) {
            if (service_send_data(x, sizeof(service_msg_header_t), (void **) &header) == -1) {
               VERBOSE(MODULE_EVENT,"%s [SERVICE] Error while sending request to module %d_%s.\n", get_formatted_time(), x, running_modules[x].module_name);
               service_disconnect_from_module(x);
            }
         }
      }

      // Update status of every module before receiving their stats
      running_modules_cnt = service_check_modules_status();

      for (x=0;x<loaded_modules_cnt;x++) {
         // Check whether the module is running and is connected with supervisor via service interface
         if (running_modules[x].module_status == TRUE && running_modules[x].module_service_ifc_isconnected == TRUE) {
            // Receive reply header
            if (service_recv_data(x, sizeof(service_msg_header_t), (void **) &header) == -1) {
               VERBOSE(MODULE_EVENT, "%s [SERVICE] Error while receiving reply header from module %d_%s.\n", get_formatted_time(), x, running_modules[x].module_name);
               service_disconnect_from_module(x);
               continue;
            }

            // Check if the reply is OK
            if (header->com != SERVICE_OK_REPLY) {
               VERBOSE(MODULE_EVENT, "%s [SERVICE] Wrong reply from module %d_%s.\n", get_formatted_time(), x, running_modules[x].module_name);
               service_disconnect_from_module(x);
               continue;
            }

            if (header->data_size > buffer_size) {
               // Reallocate buffer for incoming data
               buffer_size += (header->data_size - buffer_size) + 1;
               buffer = (char *) realloc(buffer, buffer_size * sizeof(char));
            }
            memset(buffer, 0, buffer_size * sizeof(char));

            // Receive module stats in json format
            if (service_recv_data(x, header->data_size, (void **) &buffer) == -1) {
               VERBOSE(MODULE_EVENT, "%s [SERVICE] Error while receiving stats from module %d_%s.\n", get_formatted_time(), x, running_modules[x].module_name);
               service_disconnect_from_module(x);
               continue;
            }

            // Decode json and save stats into module structure
            if (service_decode_module_stats(&buffer, x) == -1) {
               VERBOSE(MODULE_EVENT, "%s [SERVICE] Error while receiving stats from module %d_%s.\n", get_formatted_time(), x, running_modules[x].module_name);
               service_disconnect_from_module(x);
               continue;
            }
         }
      }

      pthread_mutex_unlock(&running_modules_lock);

      if ((period_cnt%30 == 0) && (running_modules_cnt > 0)) {
         print_statistics();
      }

      if (service_thread_continue == TRUE) {
         usleep(SERVICE_THREAD_SLEEP_IN_MICSEC);
      }

      period_cnt++;
   } // Service thread loop

   // Disconnect from running modules
   for (x=0;x<loaded_modules_cnt;x++) {
      service_disconnect_from_module(x);
   }

   NULLP_TEST_AND_FREE(buffer)
   NULLP_TEST_AND_FREE(header)

   pthread_exit(EXIT_SUCCESS);
}

int service_decode_module_stats(char **data, int module_idx)
{
   uint x = 0;
   int actual_ifc_index = 0;
   size_t arr_idx = 0;

   json_error_t error;
   json_t *json_struct = NULL;

   json_t *in_ifces_arr = NULL;
   json_t *out_ifces_arr = NULL;
   json_t *in_ifc_cnts  = NULL;
   json_t *out_ifc_cnts = NULL;
   json_t *cnt = NULL;

   /***********************************/

   // Parse received modules counters in json format
   json_struct = json_loads(*data , 0, &error);
    if (json_struct == NULL) {
        VERBOSE(MODULE_EVENT, "%s [ERROR] Could not convert modules (%s) stats to json structure on line %d: %s\n", get_formatted_time(), running_modules[module_idx].module_name, error.line, error.text);
        return -1;
    }

    // Check whether the root elem is a json object
    if (json_is_object(json_struct) == 0) {
      VERBOSE(MODULE_EVENT, "%s [ERROR] Root elem is not a json object (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
      json_decref(json_struct);
      return -1;
    }


    if (running_modules[module_idx].module_num_in_ifc > 0) {
      // Get value of the key "in" from json root elem (it should be an array of json objects - every object contains counters of one input interface)
      in_ifces_arr = json_object_get(json_struct, "in");
      if (in_ifces_arr == NULL) {
         VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"in\" from root json object while parsing modules stats (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
         json_decref(json_struct);
         return -1;
      }

      if (json_is_array(in_ifces_arr) == 0) {
         VERBOSE(MODULE_EVENT, "%s [ERROR] Value of key \"in\" is not a json array (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
         json_decref(json_struct);
         return -1;
      }

      actual_ifc_index = -1;
      json_array_foreach(in_ifces_arr, arr_idx, in_ifc_cnts) {
         // Find index of next input interface in modules structure
         for (x = actual_ifc_index + 1; x < running_modules[module_idx].module_ifces_cnt; x++) {
            if (running_modules[module_idx].module_ifces[x].int_ifc_direction == IN_MODULE_IFC_DIRECTION) {
               actual_ifc_index = x;
               break;
            }
         }

         if (json_is_object(in_ifc_cnts) == 0) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Counters of an input interface are not a json object in received json structure (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }

         cnt = json_object_get(in_ifc_cnts, "messages");
         if (cnt == NULL) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"%s\" from an input interface json object (module %s).\n", get_formatted_time(), "messages", running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }
         ((in_ifc_stats_t *) running_modules[module_idx].module_ifces[actual_ifc_index].ifc_data)->recv_msg_cnt = json_integer_value(cnt);

         cnt = json_object_get(in_ifc_cnts, "buffers");
         if (cnt == NULL) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"%s\" from an input interface json object (module %s).\n", get_formatted_time(), "buffers", running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }
         ((in_ifc_stats_t *) running_modules[module_idx].module_ifces[actual_ifc_index].ifc_data)->recv_buffer_cnt = json_integer_value(cnt);
      }
   }


   if (running_modules[module_idx].module_num_out_ifc > 0) {
      // Get value of the key "out" from json root elem (it should be an array of json objects - every object contains counters of one output interface)
      out_ifces_arr = json_object_get(json_struct, "out");
      if (out_ifces_arr == NULL) {
         VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"out\" from root json object while parsing modules stats (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
         json_decref(json_struct);
         return -1;
      }

      if (json_is_array(out_ifces_arr) == 0) {
         VERBOSE(MODULE_EVENT, "%s [ERROR] Value of key \"out\" is not a json array (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
         json_decref(json_struct);
         return -1;
      }

      actual_ifc_index = -1;
      json_array_foreach(out_ifces_arr, arr_idx, out_ifc_cnts) {
         // Find index of next output interface in modules structure
         for (x = actual_ifc_index + 1; x < running_modules[module_idx].module_ifces_cnt; x++) {
            if (running_modules[module_idx].module_ifces[x].int_ifc_direction == OUT_MODULE_IFC_DIRECTION) {
               actual_ifc_index = x;
               break;
            }
         }

         if (json_is_object(out_ifc_cnts) == 0) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Counters of an output interface are not a json object in received json structure (module %s).\n", get_formatted_time(), running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }

         cnt = json_object_get(out_ifc_cnts, "sent-messages");
         if (cnt == NULL) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"%s\" from an output interface json object (module %s).\n", get_formatted_time(), "sent-messages", running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }
         ((out_ifc_stats_t *) running_modules[module_idx].module_ifces[actual_ifc_index].ifc_data)->sent_msg_cnt = json_integer_value(cnt);

         cnt = json_object_get(out_ifc_cnts, "dropped-messages");
         if (cnt == NULL) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"%s\" from an output interface json object (module %s).\n", get_formatted_time(), "dropped-messages", running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }
         ((out_ifc_stats_t *) running_modules[module_idx].module_ifces[actual_ifc_index].ifc_data)->dropped_msg_cnt = json_integer_value(cnt);

         cnt = json_object_get(out_ifc_cnts, "buffers");
         if (cnt == NULL) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"%s\" from an output interface json object (module %s).\n", get_formatted_time(), "buffers", running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }
         ((out_ifc_stats_t *) running_modules[module_idx].module_ifces[actual_ifc_index].ifc_data)->sent_buffer_cnt = json_integer_value(cnt);

         cnt = json_object_get(out_ifc_cnts, "autoflushes");
         if (cnt == NULL) {
            VERBOSE(MODULE_EVENT, "%s [ERROR] Could not get key \"%s\" from an output interface json object (module %s).\n", get_formatted_time(), "autoflushes", running_modules[module_idx].module_name);
            json_decref(json_struct);
            return -1;
         }
         ((out_ifc_stats_t *) running_modules[module_idx].module_ifces[actual_ifc_index].ifc_data)->autoflush_cnt = json_integer_value(cnt);
      }
   }

   json_decref(json_struct);
   return 0;
}



/*****************************************************************
 * Interactive methods *
 *****************************************************************/

void interactive_show_available_modules()
{
   unsigned int x = 0, y = 0, already_printed_modules = 0;
   modules_profile_t * ptr = first_profile_ptr;

   if (loaded_modules_cnt == 0) {
      VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] No module is loaded.\n" FORMAT_RESET);
      return;
   }

   VERBOSE(N_STDOUT,"--- [PRINTING CONFIGURATION] ---\n");

   while (ptr != NULL) {
      if (ptr->profile_enabled == TRUE) {
         VERBOSE(N_STDOUT, FORMAT_BOLD "Profile: %s (" FORMAT_RUNNING "enabled" FORMAT_RESET ")\n", ptr->profile_name);
      } else {
         VERBOSE(N_STDOUT, FORMAT_BOLD "Profile: %s (" FORMAT_STOPPED "disabled" FORMAT_RESET ")\n" FORMAT_RESET, ptr->profile_name);
      }
      for (x = 0; x < loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile != NULL) {
            if (running_modules[x].modules_profile == ptr) {
               if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == TRUE) {
                  VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_RUNNING "enabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
               } else if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == FALSE) {
                  VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_STOPPED "disabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
               } else if (running_modules[x].module_status == FALSE && running_modules[x].module_enabled == TRUE) {
                  VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_RUNNING "enabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
               } else {
                  VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_STOPPED "disabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
               }
               VERBOSE(N_STDOUT, "      " FORMAT_BOLD "PATH:" FORMAT_RESET " %s\n", (running_modules[x].module_path == NULL ? "none" : running_modules[x].module_path));
               VERBOSE(N_STDOUT, "      " FORMAT_BOLD "PARAMS:" FORMAT_RESET " %s\n", (running_modules[x].module_params == NULL ? "none" : running_modules[x].module_params));
               for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
                  VERBOSE(N_STDOUT,"      " FORMAT_BOLD "IFC%d:" FORMAT_RESET "  %s; %s; %s; %s\n", y, (running_modules[x].module_ifces[y].ifc_direction == NULL ? "none" : running_modules[x].module_ifces[y].ifc_direction),
                                                                                                   (running_modules[x].module_ifces[y].ifc_type == NULL ? "none" : running_modules[x].module_ifces[y].ifc_type),
                                                                                                   (running_modules[x].module_ifces[y].ifc_params == NULL ? "none" : running_modules[x].module_ifces[y].ifc_params),
                                                                                                   (running_modules[x].module_ifces[y].ifc_note == NULL ? "none" : running_modules[x].module_ifces[y].ifc_note));
               }
               already_printed_modules++;
            }
         }
      }
      ptr = ptr->next;
   }

   if (already_printed_modules < loaded_modules_cnt) {
      VERBOSE(N_STDOUT, FORMAT_BOLD "Modules without profile:\n" FORMAT_RESET);
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile == NULL) {
            if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == TRUE) {
               VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_RUNNING "enabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
            } else if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == FALSE) {
               VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_STOPPED "disabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
            } else if (running_modules[x].module_status == FALSE && running_modules[x].module_enabled == TRUE) {
               VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_RUNNING "enabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
            } else {
               VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s (" FORMAT_STOPPED "disabled" FORMAT_RESET "):\n",x, running_modules[x].module_name);
            }
            VERBOSE(N_STDOUT, "      " FORMAT_BOLD "PATH:" FORMAT_RESET " %s\n", (running_modules[x].module_path == NULL ? "none" : running_modules[x].module_path));
            VERBOSE(N_STDOUT, "      " FORMAT_BOLD "PARAMS:" FORMAT_RESET " %s\n", (running_modules[x].module_params == NULL ? "none" : running_modules[x].module_params));
            for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
               VERBOSE(N_STDOUT,"      " FORMAT_BOLD "IFC%d:" FORMAT_RESET "  %s; %s; %s; %s\n", y, (running_modules[x].module_ifces[y].ifc_direction == NULL ? "none" : running_modules[x].module_ifces[y].ifc_direction),
                                                                                                (running_modules[x].module_ifces[y].ifc_type == NULL ? "none" : running_modules[x].module_ifces[y].ifc_type),
                                                                                                (running_modules[x].module_ifces[y].ifc_params == NULL ? "none" : running_modules[x].module_ifces[y].ifc_params),
                                                                                                (running_modules[x].module_ifces[y].ifc_note == NULL ? "none" : running_modules[x].module_ifces[y].ifc_note));
            }
         }
      }
   }
}

int interactive_get_option()
{
   usleep(50000); // Solved bugged output - without this sleep, escape codes in output were not sometimes reseted on time and they were applied also on this menu
   VERBOSE(N_STDOUT, FORMAT_MENU FORMAT_BOLD "--------OPTIONS--------\n" FORMAT_RESET);
   VERBOSE(N_STDOUT, FORMAT_MENU "1. ENABLE ALL MODULES\n");
   VERBOSE(N_STDOUT, "2. DISABLE ALL MODULES\n");
   VERBOSE(N_STDOUT, "3. ENABLE MODULE OR PROFILE\n");
   VERBOSE(N_STDOUT, "4. DISABLE MODULE OR PROFILE\n");
   VERBOSE(N_STDOUT, "5. CONFIGURATION STATUS\n");
   VERBOSE(N_STDOUT, "6. AVAILABLE MODULES\n");
   VERBOSE(N_STDOUT, "7. RELOAD CONFIGURATION\n");
   VERBOSE(N_STDOUT, "8. PRINT SUPERVISOR INFO\n");
   VERBOSE(N_STDOUT, "9. SHOW LOGS\n");
   VERBOSE(N_STDOUT, "0. STOP SUPERVISOR\n" FORMAT_RESET);
   VERBOSE(N_STDOUT, FORMAT_INTERACTIVE "[INTERACTIVE] Your choice: " FORMAT_RESET);

   return get_number_from_input_choosing_option();
}

void interactive_start_configuration()
{
   pthread_mutex_lock(&running_modules_lock);
   VERBOSE(MODULE_EVENT,"%s [START] Starting configuration...\n", get_formatted_time());
   unsigned int x = 0;
   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_enabled == FALSE) {
         running_modules[x].module_restart_cnt = -1;
         running_modules[x].module_enabled = TRUE;
      }
   }
   pthread_mutex_unlock(&running_modules_lock);
}


void interactive_stop_configuration()
{
   unsigned int x = 0;
   pthread_mutex_lock(&running_modules_lock);
   VERBOSE(MODULE_EVENT,"%s [STOP] Stopping configuration...\n", get_formatted_time());
   for (x=0; x<loaded_modules_cnt; x++) {
      if (running_modules[x].module_enabled) {
         running_modules[x].module_enabled = FALSE;
      }
   }
   pthread_mutex_unlock(&running_modules_lock);
}

int get_num_disabled_modules()
{
   int cnt = 0, x = 0;
   for (x = 0; x < loaded_modules_cnt; x++) {
      if (running_modules[x].module_enabled == FALSE) {
         cnt++;
      }
   }

   return cnt;
}

int get_num_disabled_profiles()
{
   int cnt = 0;
   modules_profile_t *ptr = first_profile_ptr;

   while (ptr != NULL) {
      if (ptr->profile_enabled == FALSE) {
         cnt++;
      }
      ptr = ptr->next;
   }

   return cnt;
}

void interactive_set_enabled()
{
   int mod_to_en = 0;

   uint32_t dis_prof_cnt = get_num_disabled_profiles();
   uint32_t dis_mod_cnt = get_num_disabled_modules();

   int *modules_to_enable = NULL;
   int x = 0, modules_to_enable_cnt = 0, matched_modules = 0, label_printed = FALSE;
   modules_profile_t * ptr = first_profile_ptr;
   int max_idx = 0;

   pthread_mutex_lock(&running_modules_lock);

   VERBOSE(N_STDOUT, "--- [LIST OF DISABLED MODULES] ---\n");
   if (loaded_modules_cnt == 0) {
      VERBOSE(N_STDOUT, "   No module is loaded.\n");
      goto prof_check;
   }
   // Check whether any module is disabled
   if (dis_mod_cnt == 0) {
      VERBOSE(N_STDOUT, "   All modules are enabled.\n");
      goto prof_check;
   }

   // Find modules with profile
   while (ptr != NULL) {
      label_printed = FALSE;
      for (x = 0; x < loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile != NULL) {
            if (running_modules[x].modules_profile == ptr) {
               if (running_modules[x].module_enabled == FALSE) {
                  if (label_printed == FALSE) {
                     VERBOSE(N_STDOUT, FORMAT_BOLD "Profile: %s\n" FORMAT_RESET, ptr->profile_name);
                     label_printed = TRUE;
                  }
                  VERBOSE(N_STDOUT, "   " FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET "\n", x, running_modules[x].module_name);
               }
               matched_modules++;
            }
         }
      }
      ptr = ptr->next;
   }

   // Find modules without profile 
   if (matched_modules < loaded_modules_cnt) {
      label_printed = FALSE;
      for (x = 0; x < loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile == NULL) {
            if (running_modules[x].module_enabled == FALSE) {
               if (label_printed == FALSE) {
                  VERBOSE(N_STDOUT, FORMAT_BOLD "Modules without profile:\n" FORMAT_RESET);
                  label_printed = TRUE;
               }
               VERBOSE(N_STDOUT, "   " FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET "\n", x, running_modules[x].module_name);
            }
         }
      }
   }


prof_check:
   VERBOSE(N_STDOUT, "--- [LIST OF DISABLED PROFILES] ---\n");
   if (loaded_profile_cnt == 0) {
      VERBOSE(N_STDOUT, "   No profile is loaded.\n");
      goto user_input;
   }
   // Check whether any profile is disabled
   if (dis_prof_cnt == 0) {
      VERBOSE(N_STDOUT, "   All profiles are enabled.\n");
      goto user_input;
   }

   max_idx = loaded_modules_cnt;
   ptr = first_profile_ptr;
   while (ptr != NULL) {
      if (ptr->profile_enabled == FALSE) {
         VERBOSE(N_STDOUT, "   " FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET "\n", max_idx, ptr->profile_name);
      }
      max_idx++;
      ptr = ptr->next;
   }


user_input:
   if (dis_prof_cnt == 0 && dis_mod_cnt == 0) {
      // There is no module nor profile that can be enabled
      pthread_mutex_unlock(&running_modules_lock);
      return;
   }

   VERBOSE(N_STDOUT, FORMAT_INTERACTIVE "[INTERACTIVE] Type in number or interval separated by comma (e.g. \"2,4-6,13\"): " FORMAT_RESET);
   modules_to_enable_cnt = parse_numbers_user_selection(&modules_to_enable);

   if (modules_to_enable_cnt != RET_ERROR) {
      for (x = 0; x < modules_to_enable_cnt; x++) {
         mod_to_en = modules_to_enable[x];

         if ((mod_to_en >= (loaded_modules_cnt + loaded_profile_cnt)) || mod_to_en < 0) {
            VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Number %d is not in range <0,%d>!\n" FORMAT_RESET, mod_to_en, (loaded_modules_cnt + loaded_profile_cnt - 1));
            continue;
         } else if (mod_to_en >= loaded_modules_cnt) {
            mod_to_en -= loaded_modules_cnt;
            ptr = first_profile_ptr;
            while (ptr != NULL) {
               if (mod_to_en == 0) {
                  if (ptr->profile_enabled == FALSE) {
                     VERBOSE(MODULE_EVENT, "%s [ENABLED] Profile %s set to enabled.\n", get_formatted_time(), ptr->profile_name);
                     ptr->profile_enabled = TRUE;
                  }
                  break;
               }
               mod_to_en--;
               ptr = ptr->next;
            }
         } else if (running_modules[mod_to_en].module_enabled == TRUE) {
            VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Module %s is already enabled.\n" FORMAT_RESET, running_modules[mod_to_en].module_name);
         } else {
            running_modules[mod_to_en].module_enabled = TRUE;
            running_modules[mod_to_en].module_restart_cnt = -1;
            VERBOSE(MODULE_EVENT, "%s [ENABLED] Module %s set to enabled.\n", get_formatted_time(), running_modules[mod_to_en].module_name);
         }
      }
      free(modules_to_enable);
      modules_to_enable = NULL;
   }

   pthread_mutex_unlock(&running_modules_lock);
}

void interactive_set_disabled()
{
   int mod_to_dis = 0;

   uint32_t en_prof_cnt = loaded_profile_cnt - get_num_disabled_profiles();
   uint32_t en_mod_cnt = loaded_modules_cnt - get_num_disabled_modules();

   int *modules_to_disable = NULL;
   int x = 0, modules_to_disable_cnt = 0, matched_modules = 0, label_printed = FALSE;
   modules_profile_t * ptr = first_profile_ptr;
   int max_idx = 0;

   pthread_mutex_lock(&running_modules_lock);

   VERBOSE(N_STDOUT, "--- [LIST OF ENABLED MODULES] ---\n");
   if (loaded_modules_cnt == 0) {
      VERBOSE(N_STDOUT, "   No module is loaded.\n");
      goto prof_check;
   }
   // Check whether any module is disabled
   if (en_mod_cnt == 0) {
      VERBOSE(N_STDOUT, "   All modules are disabled.\n");
      goto prof_check;
   }

   // Find modules with profile
   while (ptr != NULL) {
      label_printed = FALSE;
      for (x = 0; x < loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile != NULL) {
            if (running_modules[x].modules_profile == ptr) {
               if (running_modules[x].module_enabled == TRUE) {
                  if (label_printed == FALSE) {
                     VERBOSE(N_STDOUT, FORMAT_BOLD "Profile: %s\n" FORMAT_RESET, ptr->profile_name);
                     label_printed = TRUE;
                  }
                  VERBOSE(N_STDOUT, "   " FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET "\n", x, running_modules[x].module_name);
               }
               matched_modules++;
            }
         }
      }
      ptr = ptr->next;
   }

   // Find modules without profile 
   if (matched_modules < loaded_modules_cnt) {
      label_printed = FALSE;
      for (x = 0; x < loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile == NULL) {
            if (running_modules[x].module_enabled == TRUE) {
               if (label_printed == FALSE) {
                  VERBOSE(N_STDOUT, FORMAT_BOLD "Modules without profile:\n" FORMAT_RESET);
                  label_printed = TRUE;
               }
               VERBOSE(N_STDOUT, "   " FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET "\n", x, running_modules[x].module_name);
            }
         }
      }
   }


prof_check:
   VERBOSE(N_STDOUT, "--- [LIST OF ENABLED PROFILES] ---\n");
   if (loaded_profile_cnt == 0) {
      VERBOSE(N_STDOUT, "   No profile is loaded.\n");
      goto user_input;
   }
   // Check whether any profile is enabled
   if (en_prof_cnt == 0) {
      VERBOSE(N_STDOUT, "   All profiles are disabled.\n");
      goto user_input;
   }

   max_idx = loaded_modules_cnt;
   ptr = first_profile_ptr;
   while (ptr != NULL) {
      if (ptr->profile_enabled == TRUE) {
         VERBOSE(N_STDOUT, "   " FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET "\n", max_idx, ptr->profile_name);
      }
      max_idx++;
      ptr = ptr->next;
   }


user_input:
   if (en_prof_cnt == 0 && en_mod_cnt == 0) {
      // There is no module nor profile that can be disabled
      pthread_mutex_unlock(&running_modules_lock);
      return;
   }

   VERBOSE(N_STDOUT, FORMAT_INTERACTIVE "[INTERACTIVE] Type in number or interval separated by comma (e.g. \"2,4-6,13\"): " FORMAT_RESET);
   modules_to_disable_cnt = parse_numbers_user_selection(&modules_to_disable);

   if (modules_to_disable_cnt != RET_ERROR) {
      for (x = 0; x < modules_to_disable_cnt; x++) {
         mod_to_dis = modules_to_disable[x];

         if ((mod_to_dis >= (loaded_modules_cnt + loaded_profile_cnt)) || mod_to_dis < 0) {
            VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Number %d is not in range <0,%d>!\n" FORMAT_RESET, mod_to_dis, (loaded_modules_cnt + loaded_profile_cnt - 1));
            continue;
         } else if (mod_to_dis >= loaded_modules_cnt) {
            mod_to_dis -= loaded_modules_cnt;
            ptr = first_profile_ptr;
            while (ptr != NULL) {
               if (mod_to_dis == 0) {
                  if (ptr->profile_enabled == TRUE) {
                     VERBOSE(MODULE_EVENT, "%s [ENABLED] Profile %s set to disabled.\n", get_formatted_time(), ptr->profile_name);
                     ptr->profile_enabled = FALSE;
                  }
                  break;
               }
               mod_to_dis--;
               ptr = ptr->next;
            }
         } else if (running_modules[mod_to_dis].module_enabled == FALSE) {
            VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Module %s is already disabled.\n" FORMAT_RESET, running_modules[mod_to_dis].module_name);
         } else {
            running_modules[mod_to_dis].module_enabled = FALSE;
            VERBOSE(MODULE_EVENT, "%s [ENABLED] Module %s set to disabled.\n", get_formatted_time(), running_modules[mod_to_dis].module_name);
         }
      }
      free(modules_to_disable);
      modules_to_disable = NULL;
   }

   pthread_mutex_unlock(&running_modules_lock);
}

void interactive_show_logs()
{
   // format vars
   int log_idx_dig_num = 1;
   int log_idx_rank = 1;
   int char_pos = 0;
   // (stdout + stderr) * modules_cnt + sup_log + sup_log_stats + sup_log_modules_events
   uint16_t max_num_of_logs = (2 * loaded_modules_cnt) + 3;
   uint8_t avail_logs[max_num_of_logs];
   memset(avail_logs, 0, max_num_of_logs * sizeof(uint8_t));
   int x = 0, log_idx = -1, chosen_log_idx = 0;
   char *file_path = (char *) calloc(PATH_MAX, sizeof(char));
   memset(file_path, 0, PATH_MAX);
   char *file_path_ptr = file_path + strlen(logs_path);

   if (sprintf(file_path, "%s", logs_path) < 1) {
      VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
      goto exit_label;
   }

   VERBOSE(N_STDOUT, FORMAT_BOLD "Available modules logs:" FORMAT_RESET "\n");
   VERBOSE(N_STDOUT, "   " FORMAT_BOLD "stdout" FORMAT_RESET " | " FORMAT_BOLD "stderr" FORMAT_RESET " | " FORMAT_BOLD "module name" FORMAT_RESET "\n");

   for (x = 0; x < loaded_modules_cnt; x++) {
      log_idx++;
      // Get the number of log_idx digits
      if (log_idx >= (log_idx_rank * 10)) {
         log_idx_dig_num++;
         log_idx_rank*=10;
      }

      // Test module's stdout log
      if (sprintf(file_path_ptr, "modules_logs/%s_stdout", running_modules[x].module_name) < 1) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
         goto exit_label;
      }
      file_path_ptr += strlen("modules_logs/_std") + strlen(running_modules[x].module_name);
      if (access(file_path, R_OK) != 0) {
         VERBOSE(N_STDOUT, "   " FORMAT_STOPPED "%d" FORMAT_RESET, log_idx);
         avail_logs[log_idx] = FALSE;
      } else {
         VERBOSE(N_STDOUT, "   " FORMAT_RUNNING "%d" FORMAT_RESET, log_idx);
         avail_logs[log_idx] = TRUE;
      }

      // Align the stderr column
      char_pos = 3 + log_idx_dig_num;
      VERBOSE(N_STDOUT, "%*c| ", (10 - char_pos), ' ');
      char_pos += (10 - char_pos) + 2;

      log_idx++;
      // Test module's stderr log
      if (sprintf(file_path_ptr, "err") < 1) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
         goto exit_label;
      }
      file_path_ptr = file_path + strlen(logs_path);
      if (access(file_path, R_OK) != 0) {
         VERBOSE(N_STDOUT, FORMAT_STOPPED "%d" FORMAT_RESET, log_idx);
         avail_logs[log_idx] = FALSE;
      } else {
         VERBOSE(N_STDOUT, FORMAT_RUNNING "%d" FORMAT_RESET, log_idx);
         avail_logs[log_idx] = TRUE;
      }

      // ALign the module name column
      char_pos += log_idx_dig_num;
      VERBOSE(N_STDOUT, "%*c| %s\n", (19 - char_pos), ' ', running_modules[x].module_name);

      // Zero the rest of file_path memory after "logs_path"
      memset(file_path + strlen(logs_path), 0, (PATH_MAX - strlen(logs_path)) * sizeof(char));
   }

   VERBOSE(N_STDOUT, FORMAT_BOLD "Available supervisor logs:" FORMAT_RESET "\n");

   log_idx++;
   // Test the supervisor_log file
   if (sprintf(file_path_ptr, "supervisor_log") < 1) {
      VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
      goto exit_label;
   }
   if (access(file_path, R_OK) != 0) {
      VERBOSE(N_STDOUT,"   " FORMAT_STOPPED "%d" FORMAT_RESET " | supervisor_log\n", log_idx);
      avail_logs[log_idx] = FALSE;
   } else {
      VERBOSE(N_STDOUT,"   " FORMAT_RUNNING "%d" FORMAT_RESET " | supervisor_log\n", log_idx);
      avail_logs[log_idx] = TRUE;
   }

   log_idx++;
   // Test the supervisor_log_statistics file (no need to erase file_path string memory - it is overwritten)
   if (sprintf(file_path_ptr, "supervisor_log_statistics") < 1) {
      VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
      goto exit_label;
   }
   if (access(file_path, R_OK) != 0) {
      VERBOSE(N_STDOUT,"   " FORMAT_STOPPED "%d" FORMAT_RESET " | supervisor_log_statistics\n", log_idx);
      avail_logs[log_idx] = FALSE;
   } else {
      VERBOSE(N_STDOUT,"   " FORMAT_RUNNING "%d" FORMAT_RESET " | supervisor_log_statistics\n", log_idx);
      avail_logs[log_idx] = TRUE;
   }

   log_idx++;
   // Test the supervisor_log_module_event file (no need to erase file_path string memory - it is overwritten)
   if (sprintf(file_path_ptr, "supervisor_log_module_event") < 1) {
      VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
      goto exit_label;
   }
   if (access(file_path, R_OK) != 0) {
      VERBOSE(N_STDOUT,"   " FORMAT_STOPPED "%d" FORMAT_RESET " | supervisor_log_module_event\n", log_idx);
      avail_logs[log_idx] = FALSE;
   } else {
      VERBOSE(N_STDOUT,"   " FORMAT_RUNNING "%d" FORMAT_RESET " | supervisor_log_module_event\n", log_idx);
      avail_logs[log_idx] = TRUE;
   }

   VERBOSE(N_STDOUT, FORMAT_INTERACTIVE "[INTERACTIVE] Choose the log number: " FORMAT_RESET);
   chosen_log_idx = get_number_from_input_choosing_option();
   if (chosen_log_idx == -1 || chosen_log_idx > max_num_of_logs) {
      VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] Wrong input.\n" FORMAT_RESET);
      goto exit_label;
   }

   if (avail_logs[chosen_log_idx] == FALSE) {
      VERBOSE(N_STDOUT, FORMAT_WARNING "[ERROR] Chosen log is not available\n" FORMAT_RESET);
      goto exit_label;
   }

   memset(file_path, 0, PATH_MAX * sizeof(char));
   if (chosen_log_idx < (max_num_of_logs - 3)) {
      if (chosen_log_idx % 2 == 0) {
         // stdout
         if (sprintf(file_path, "%smodules_logs/%s_stdout", logs_path, running_modules[chosen_log_idx/2].module_name) < 1) {
            VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
            goto exit_label;
         }
      } else {
         // stderr
         if (sprintf(file_path, "%smodules_logs/%s_stderr", logs_path, running_modules[chosen_log_idx/2].module_name) < 1) {
            VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
            goto exit_label;
         }
      }
   } else if (chosen_log_idx == (max_num_of_logs - 3)) {
      if (sprintf(file_path, "%ssupervisor_log", logs_path) < 1) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
         goto exit_label;
      }
   } else if (chosen_log_idx == (max_num_of_logs - 2)) {
      if (sprintf(file_path, "%ssupervisor_log_statistics", logs_path) < 1) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
         goto exit_label;
      }
   } else {
      if (sprintf(file_path, "%ssupervisor_log_module_event", logs_path) < 1) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create log file path.\n");
         goto exit_label;
      }
   }

   if (daemon_flag == TRUE) {
      // Send the log file path to client via tmp file and it afterwards executes the pager
      FILE *tmp_file = fopen(SUP_CLI_TMP_FILE, "w");
      if (tmp_file == NULL) {
         VERBOSE(N_STDOUT, "[ERROR] Could not deliver log file path to the supervisor client via /tmp/tmp_sup_cli_file.\n");
         goto exit_label;
      } else {
         fprintf(tmp_file, "%d\n%s", (int) strlen(file_path), file_path);
         fflush(tmp_file);
         fclose(tmp_file);
      }
      goto exit_label;
   } else {
      show_file_with_pager(&file_path);
   }

exit_label:
   NULLP_TEST_AND_FREE(file_path)
   return;
}

void interactive_show_running_modules_status()
{
   unsigned int x = 0, already_printed_modules = 0;
   modules_profile_t * ptr = first_profile_ptr;

   if (loaded_modules_cnt == 0) {
      VERBOSE(N_STDOUT, FORMAT_WARNING "[WARNING] No module is loaded.\n" FORMAT_RESET);
      return;
   }

   VERBOSE(N_STDOUT, "--- [CONFIGURATION STATUS] ---\n");

   while (ptr != NULL) {
      if (ptr->profile_enabled == TRUE) {
         VERBOSE(N_STDOUT, FORMAT_BOLD "Profile: %s (" FORMAT_RUNNING "enabled" FORMAT_RESET ")\n", ptr->profile_name);
      } else {
         VERBOSE(N_STDOUT, FORMAT_BOLD "Profile: %s (" FORMAT_STOPPED "disabled" FORMAT_RESET ")\n" FORMAT_RESET, ptr->profile_name);
      }
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile != NULL) {
            if (running_modules[x].modules_profile == ptr) {
               if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == TRUE) {
                  VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
                  already_printed_modules++;
               } else if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == FALSE) {
                  VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
                  already_printed_modules++;
               } else if (running_modules[x].module_status == FALSE && running_modules[x].module_enabled == TRUE) {
                  VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
                  already_printed_modules++;
               } else {
                  VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
                  already_printed_modules++;
               }
            }
         }
      }
      ptr = ptr->next;
   }

   if (already_printed_modules < loaded_modules_cnt) {
      VERBOSE(N_STDOUT, FORMAT_BOLD "Modules without profile:\n" FORMAT_RESET);
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].modules_profile == NULL) {
            if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == TRUE) {
                  VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
               } else if (running_modules[x].module_status == TRUE && running_modules[x].module_enabled == FALSE) {
                  VERBOSE(N_STDOUT, FORMAT_RUNNING"   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
               } else if (running_modules[x].module_status == FALSE && running_modules[x].module_enabled == TRUE) {
                  VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_RUNNING "enabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
               } else {
                  VERBOSE(N_STDOUT, FORMAT_STOPPED "   ⚫ " FORMAT_RESET FORMAT_BOLD "%d" FORMAT_RESET " | %s " FORMAT_STOPPED "disabled" FORMAT_RESET " (PID: %d)\n",x, running_modules[x].module_name,running_modules[x].module_pid);
               }
         }
      }
   }
}

void interactive_print_supervisor_info()
{
   VERBOSE(N_STDOUT, FORMAT_BOLD "--------------- INFO ---------------\n");
   VERBOSE(N_STDOUT, "Supervisor package version:" FORMAT_RESET " %s\n", sup_package_version);
   VERBOSE(N_STDOUT, FORMAT_BOLD "Supervisor git version:" FORMAT_RESET " %s\n", sup_git_version);
   VERBOSE(N_STDOUT, FORMAT_BOLD "Started:" FORMAT_RESET " %s", ctime(&sup_init_time));
   VERBOSE(N_STDOUT, FORMAT_BOLD "Actual logs directory:" FORMAT_RESET " %s\n", get_absolute_file_path(logs_path));
   VERBOSE(N_STDOUT, FORMAT_BOLD "Start-up configuration file:" FORMAT_RESET " %s\n", get_absolute_file_path(templ_config_file));
   VERBOSE(N_STDOUT, FORMAT_BOLD "Number of loaded modules:" FORMAT_RESET " %d\n", loaded_modules_cnt);
   VERBOSE(N_STDOUT, FORMAT_BOLD "Number of running modules:" FORMAT_RESET " %d\n", service_check_modules_status());
}


/*****************************************************************
 * Supervisor termination and clean up functions *
 *****************************************************************/

void free_module_on_index(const int module_idx)
{
   free_module_interfaces_on_index(module_idx);

   NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces)
   NULLP_TEST_AND_FREE(running_modules[module_idx].module_path)
   NULLP_TEST_AND_FREE(running_modules[module_idx].module_name)
   NULLP_TEST_AND_FREE(running_modules[module_idx].module_params)
}

void free_module_interfaces_on_index(const int module_idx)
{
   unsigned int y;
   for (y=0; y<running_modules[module_idx].module_ifces_cnt; y++) {
      NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces[y].ifc_note)
      NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces[y].ifc_type)
      NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces[y].ifc_direction)
      NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces[y].ifc_params)
      NULLP_TEST_AND_FREE(running_modules[module_idx].module_ifces[y].ifc_data)
   }
}

void free_output_file_strings_and_streams()
{
   NULLP_TEST_AND_FREE(statistics_file_path)
   NULLP_TEST_AND_FREE(module_event_file_path)
   NULLP_TEST_AND_FREE(supervisor_debug_log_file_path)
   NULLP_TEST_AND_FREE(supervisor_log_file_path)

   if (supervisor_debug_log_fd != NULL) {
      fclose(supervisor_debug_log_fd);
      supervisor_debug_log_fd = NULL;
   }
   if (supervisor_log_fd != NULL) {
      fclose(supervisor_log_fd);
      supervisor_log_fd = NULL;
   }
   if (statistics_fd != NULL) {
      fclose(statistics_fd);
      statistics_fd = NULL;
   }
   if (module_event_fd != NULL) {
      fclose(module_event_fd);
      module_event_fd = NULL;
   }
}

void free_module_and_shift_array(const int module_idx)
{
   int y = 0;

   free_module_on_index(module_idx);
   running_modules[module_idx].module_ifces_cnt = 0;
   running_modules[module_idx].module_num_out_ifc = 0;
   running_modules[module_idx].module_num_in_ifc = 0;
   running_modules[module_idx].module_ifces_array_size = 0;
   for (y=module_idx; y<(loaded_modules_cnt-1); y++) {
      memcpy(&running_modules[y], &running_modules[y+1], sizeof(running_module_t));
   }
   loaded_modules_cnt--;
   memset(&running_modules[loaded_modules_cnt], 0, sizeof(running_module_t));
}

void supervisor_termination(const uint8_t stop_all_modules, const uint8_t generate_backup)
{
   int x = 0, attemps = 0;

   // If daemon mode was initialized and supervisor caught a signal to terminate, set termination flag for client's threads
   if (daemon_mode_initialized  == TRUE && server_internals != NULL) {
      pthread_mutex_lock(&server_internals->lock);
      server_internals->daemon_terminated = TRUE;
      pthread_mutex_unlock(&server_internals->lock);
      if (netconf_flag == TRUE) {
         sleep(1); // Wait for server thread
      }
   }

   // If supervisor was initialized, than proceed termination, else just check allocated memory from program argument parsing
   if (supervisor_initialized == TRUE) {
      // If service thread was created successfully, check running modules, terminate service thread and (if needed) generate backup file
      if (service_thread_initialized == TRUE) {
         if (stop_all_modules == TRUE) {
            interactive_stop_configuration();
            service_stop_all_modules = TRUE;
         } else {
            service_stop_all_modules = FALSE;
         }

         VERBOSE(N_STDOUT,"%s [SERVICE] Aborting service thread!\n", get_formatted_time());
         service_thread_continue = FALSE;

         x = pthread_join(service_thread_id, NULL);

         if (x == 0) {
            VERBOSE(N_STDOUT, "%s [SERVICE] pthread_join success: Service thread finished!\n", get_formatted_time())
         } else if (x == -1) {
            if (errno == EINVAL) {
               VERBOSE(N_STDOUT, "%s [ERROR] pthread_join: Not joinable thread!\n", get_formatted_time());
            } else if (errno == ESRCH) {
               VERBOSE(N_STDOUT, "%s [ERROR] pthread_join: No thread with this ID found!\n", get_formatted_time());
            } else if ( errno == EDEADLK) {
               VERBOSE(N_STDOUT, "%s [ERROR] pthread_join: Deadlock in service thread detected!\n", get_formatted_time());
            }
         }

         if (generate_backup == TRUE) {
            generate_backup_config_file();
         } else {
            for (x = 0;  x < loaded_modules_cnt; x++) {
               if (running_modules[x].module_status == TRUE) {
                  VERBOSE(N_STDOUT, "%s [WARNING] Some modules are still running, gonna generate backup anyway!\n", get_formatted_time());
                  generate_backup_config_file();
                  break;
               }
            }
         }
      }

      for (x = 0; x < running_modules_array_size; x++) {
         free_module_on_index(x);
      }

      NULLP_TEST_AND_FREE(running_modules)

      modules_profile_t * ptr = first_profile_ptr;
      modules_profile_t * p = NULL;
      while (ptr != NULL) {
         p = ptr;
         ptr = ptr->next;
         NULLP_TEST_AND_FREE(p->profile_name)
         NULLP_TEST_AND_FREE(p)
      }
   }

   // If daemon_mode_initialization call was successful, cleanup after daemon
   if (daemon_mode_initialized == TRUE) {
      if (server_internals != NULL) {
         if (server_internals->clients != NULL) {
            // Wait for daemon clients threads
            VERBOSE(SUP_LOG, "%s [INFO] Waiting for client's threads to terminate.\n", get_formatted_time());
            for (x = 0; x < MAX_NUMBER_SUP_CLIENTS; x++) {
               // After 2 unsuccessful attempts terminate
               if (attemps >= 2) {
                  VERBOSE(SUP_LOG, "%s [INFO] Enough waiting, gonna terminate anyway.\n", get_formatted_time());
                  break;
               }
               // If any client is still connected, wait 300 ms and check all clients again
               if (server_internals->clients[x]->client_connected == TRUE) {
                  attemps++;
                  x = -1;
                  usleep(300000);
                  VERBOSE(SUP_LOG, "...\n");
               }
            }
            if (attemps < 2) {
               VERBOSE(SUP_LOG, "%s [INFO] All client's threads terminated.\n", get_formatted_time());
            }
            for (x = 0; x < MAX_NUMBER_SUP_CLIENTS; x++) {
               NULLP_TEST_AND_FREE(server_internals->clients[x])
            }
            free(server_internals->clients);
            server_internals->clients = NULL;
         }

         if (server_internals->server_sd > 0) {
            close(server_internals->server_sd);
            server_internals->server_sd = 0;
         }
         free(server_internals);
         server_internals = NULL;
         unlink(socket_path);
      }
   }

   if (supervisor_initialized == TRUE) {
      free_output_file_strings_and_streams();
   }

   NULLP_TEST_AND_FREE(templ_config_file)
   NULLP_TEST_AND_FREE(logs_path)
}



/*****************************************************************
 * Supervisor initialization functions *
 *****************************************************************/

#define CREATED_DEFAULT_LOGS   1
#define CREATED_USER_DEFINED_LOGS   2

int init_sup_logs_dir()
{
   char *buffer = NULL;
   struct stat st = {0};
   uint8_t default_path_used = FALSE;
   char modules_logs_path[PATH_MAX];
   memset(modules_logs_path, 0, PATH_MAX);

logs_path_null:
   if (logs_path == NULL) {
      default_path_used = TRUE;
      if (netconf_flag == TRUE) {
         logs_path = strdup(NETCONF_DEFAULT_LOGSDIR_PATH);
      } else if (daemon_flag == TRUE) {
         logs_path = strdup(DAEMON_DEFAULT_LOGSDIR_PATH);
      } else {
         if ((buffer = getenv("HOME")) != NULL) {
            logs_path = (char *) calloc(strlen(buffer)+strlen("/supervisor_logs/")+1, sizeof(char));
            sprintf(logs_path,"%s/supervisor_logs/", buffer);
         } else {
            logs_path = strdup(INTERACTIVE_DEFAULT_LOGSDIR_PATH);
         }
      }
   }

   if (strlen(logs_path) > 0 && logs_path[strlen(logs_path)-1] != '/') {
      buffer = (char *) calloc(strlen(logs_path)+2, sizeof(char));
      sprintf(buffer, "%s/", logs_path);
      free(logs_path);
      logs_path = buffer;
   }

   // Create modules logs path
   if (sprintf(modules_logs_path, "%smodules_logs/", logs_path) <= 0) {
      goto fail_label;
   }

   if (mkdir(logs_path, PERM_LOGSDIR) == -1) {
      if (errno == EACCES) { // Don't have permissions to some folder in logs_path, use default directory according to executed mode of supervisor
         VERBOSE(N_STDOUT, "%s [ERROR] Don't have permissions to create a directory with path \"%s\".", get_formatted_time(), logs_path);
      } else if (errno == EEXIST) { // logs_path already exists -> check whether it is a directory and create modules logs directory
         goto modules_dir;
      } else if (errno == ENOENT || errno == ENOTDIR) { // Some prefix of the logs_path is not a directory, use default directory according to executed mode of supervisor
         VERBOSE(N_STDOUT, "%s [ERROR] Some prefix of the path \"%s\" is not a directory.", get_formatted_time(), logs_path);
      }
      if (default_path_used == TRUE) { // Prevent cycling (don't need more attempts to create directory with default path)
         goto fail_label;
      } else { // Gonna create logs directory with default path
         NULLP_TEST_AND_FREE(logs_path)
         goto logs_path_null;
      }
   }

modules_dir:
   if (mkdir(modules_logs_path, PERM_LOGSDIR) == -1) {
      if (errno == EACCES) {
         VERBOSE(N_STDOUT, "%s [ERROR] Don't have permissions to create a directory with path \"%s\".", get_formatted_time(), modules_logs_path);
      } else if (errno == EEXIST) { // modules_logs_path already exists
         goto success_label;
      } else if (errno == ENOTDIR) {
         VERBOSE(N_STDOUT, "%s [ERROR] The path \"%s\" is not a directory.", get_formatted_time(), logs_path);
      }
      if (default_path_used == TRUE) { // Prevent cycling (don't need more attempts to create directory with default path)
         goto fail_label;
      } else { // Gonna create logs directory with default path
         NULLP_TEST_AND_FREE(logs_path)
         goto logs_path_null;
      }
   }

success_label:
   if (stat(modules_logs_path, &st) != -1) { // Get info about modules logs path
      if (S_ISDIR(st.st_mode) == FALSE) { // Check whether the file is a directory
         modules_logs_path_initialized = FALSE;
      } else {
         modules_logs_path_initialized = TRUE;
      }
   }
   logs_paths_initialized = TRUE;
   if (default_path_used == TRUE) {
      return CREATED_DEFAULT_LOGS;
   }
   return CREATED_USER_DEFINED_LOGS;

fail_label:
   logs_paths_initialized = FALSE;
   modules_logs_path_initialized = FALSE;
   NULLP_TEST_AND_FREE(logs_path)
   return -1;
}

void init_sup_logs_files()
{
   free_output_file_strings_and_streams();

   if (logs_path != NULL) {
      supervisor_debug_log_file_path = (char *) calloc(strlen(logs_path)+strlen("supervisor_debug_log")+1, sizeof(char));
      sprintf(supervisor_debug_log_file_path, "%ssupervisor_debug_log", logs_path);
      statistics_file_path = (char *) calloc(strlen(logs_path)+strlen("supervisor_log_statistics")+1, sizeof(char));
      sprintf(statistics_file_path, "%ssupervisor_log_statistics", logs_path);
      module_event_file_path = (char *) calloc(strlen(logs_path)+strlen("supervisor_log_module_event")+1, sizeof(char));
      sprintf(module_event_file_path, "%ssupervisor_log_module_event", logs_path);

      supervisor_debug_log_fd = fopen(supervisor_debug_log_file_path, "a");
      if (supervisor_debug_log_fd == NULL) {
         fprintf(stderr, "%s [ERROR] Could not open supervisor_debug_log file stream!\n",get_formatted_time());
      } else {
         fprintf(supervisor_debug_log_fd,"-------------------- %s --------------------\n", get_formatted_time());
      }
      statistics_fd = fopen(statistics_file_path, "a");
      if (statistics_fd == NULL) {
         fprintf(stderr, "%s [ERROR] Could not open supervisor_log_statistics file stream!\n",get_formatted_time());
      } else {
         VERBOSE(STATISTICS,"-------------------- %s --------------------\n", get_formatted_time());
         print_statistics_legend();
      }
      module_event_fd = fopen(module_event_file_path, "a");
      if (module_event_fd == NULL) {
         fprintf(stderr, "%s [ERROR] Could not open supervisor_log_module_event file stream!\n",get_formatted_time());
      } else {
         VERBOSE(MODULE_EVENT,"-------------------- %s --------------------\n", get_formatted_time());
      }

      if (netconf_flag || daemon_flag) {
         supervisor_log_file_path = (char *) calloc(strlen(logs_path)+strlen("supervisor_log")+1, sizeof(char));
         sprintf(supervisor_log_file_path, "%ssupervisor_log", logs_path);

         supervisor_log_fd = fopen (supervisor_log_file_path, "a");
         if (supervisor_log_fd == NULL) {
            fprintf(stderr, "%s [ERROR] Could not open supervisor_log file stream!\n",get_formatted_time());
         } else {
            fprintf(supervisor_log_fd,"-------------------- %s --------------------\n", get_formatted_time());
         }
         if (server_internals->clients_cnt == 0) {
            output_fd = supervisor_log_fd;
         }
      } else {
         output_fd = stdout;
      }
   }
}

void sup_sig_handler(int catched_signal)
{
   switch (catched_signal) {
   case SIGPIPE:
      break;

   case SIGTERM:
      VERBOSE(N_STDOUT,"%s [SIGNAL HANDLER] SIGTERM catched -> I'm going to terminate my self !\n", get_formatted_time());
      supervisor_termination(TRUE, FALSE);
      exit(EXIT_SUCCESS);
      break;

   case SIGINT:
      VERBOSE(N_STDOUT,"%s [SIGNAL HANDLER] SIGINT catched -> I'm going to terminate my self !\n", get_formatted_time());
      supervisor_termination(FALSE, TRUE);
      exit(EXIT_SUCCESS);
      break;

   case SIGQUIT:
      VERBOSE(N_STDOUT,"%s [SIGNAL HANDLER] SIGQUIT catched -> I'm going to terminate my self !\n", get_formatted_time());
      supervisor_termination(FALSE, TRUE);
      exit(EXIT_SUCCESS);
      break;

   case SIGSEGV:
      VERBOSE(N_STDOUT,"%s [SIGNAL HANDLER] Ouch, SIGSEGV catched -> I'm going to terminate my self !\n", get_formatted_time());
      supervisor_termination(FALSE, TRUE);
      exit(EXIT_FAILURE);
      break;
   }
}

void init_sup_flags()
{
   supervisor_initialized = FALSE;
   service_thread_initialized = FALSE;
   daemon_mode_initialized = FALSE;
   logs_paths_initialized = FALSE;
   modules_logs_path_initialized = FALSE;

   logs_path = NULL;
   templ_config_file = NULL;
   gener_config_file = NULL;
   running_config_file = NULL;
   socket_path = NULL;

   daemon_flag = FALSE;
   netconf_flag = FALSE;

   // Create temporary logs for writing - if an error occurs, it doesn't matter
   supervisor_log_fd = fopen(INIT_TMP_LOG_PATH, "w");
   supervisor_debug_log_fd = fopen(INIT_TMP_DEBUG_LOG_PATH, "w");

   // Temporarily redirect standard output to tmp file for every supervisors mode to prevent losing possible warning and error messages
   if (supervisor_log_fd != NULL) {
      output_fd = supervisor_log_fd;
   }
   input_fd = stdin;
}

void append_tmp_logs()
{
   char buffer[DEFAULT_SIZE_OF_BUFFER];
   int ret_val = 0;

   // Open temporary logs for reading and write their whole content to already created logs
   FILE * tmp_log_fd = fopen(INIT_TMP_LOG_PATH, "r");
   if (tmp_log_fd != NULL) {
      while (feof(tmp_log_fd) == FALSE) {
         ret_val = fread((void *) buffer, sizeof(char), DEFAULT_SIZE_OF_BUFFER, tmp_log_fd);
         if (ret_val > 0) {
            buffer[ret_val] = 0;
            VERBOSE(N_STDOUT, "%s", buffer);
         } else {
            break;
         }
      }
      fclose(tmp_log_fd);
      tmp_log_fd = NULL;
   }

   FILE * tmp_debug_log_fd = fopen(INIT_TMP_DEBUG_LOG_PATH, "r");
   if (tmp_debug_log_fd != NULL) {
      while (feof(tmp_debug_log_fd) == FALSE) {
         ret_val = fread((void *) buffer, sizeof(char), DEFAULT_SIZE_OF_BUFFER, tmp_debug_log_fd);
         if (ret_val > 0) {
            buffer[ret_val] = 0;
            VERBOSE(DEBUG, "%s", buffer);
         } else {
            break;
         }
      }
      fclose(tmp_debug_log_fd);
      tmp_debug_log_fd = NULL;
   }

   // Delete temporary log files
   if (unlink(INIT_TMP_LOG_PATH) == -1) {
      if (errno != ENOENT) {
         VERBOSE(N_STDOUT, "%s [WARNING] Could not delete tmp log file with path \"%s\".", get_formatted_time(), INIT_TMP_LOG_PATH);
      }
   }
   if (unlink(INIT_TMP_DEBUG_LOG_PATH) == -1) {
      if (errno != ENOENT) {
         VERBOSE(N_STDOUT, "%s [WARNING] Could not delete tmp debug log file with path \"%s\".", get_formatted_time(), INIT_TMP_DEBUG_LOG_PATH);
      }
   }
}

#define CHECK_DIR 1
#define CHECK_FILE 2

int check_file_type_perm(char *item_path, uint8_t file_type, int file_perm)
{
   struct stat st;

   if (stat(item_path, &st) == -1) {
      return -1;
   }

   if (S_ISREG(st.st_mode) == TRUE && file_type == CHECK_FILE) {
      // nothing to do here
   } else if (S_ISDIR(st.st_mode) == TRUE && file_type == CHECK_DIR) {
      // nothing to do here
   } else if (S_ISREG(st.st_mode) == FALSE && file_type == CHECK_FILE) {
      // print warning?
      return -1;
   } else if (S_ISDIR(st.st_mode) == FALSE && file_type == CHECK_DIR) {
      // print warning?
      return -1;
   } else {
      // print warning?
      return -1;
   }

   if (access(item_path, file_perm) == -1) {
      // print warning?
      return -1;
   }

   return 0;
}

int init_files()
{
   if (config_files_path != NULL) {
         if (check_file_type_perm(config_files_path, CHECK_DIR, R_OK | W_OK) == -1) {
            VERBOSE(N_STDOUT, "[ERROR] Path for configuration files \"%s\" is not a directory or it has wrong permissions (read and write needed).\n", config_files_path);
            return -1;
         }
         gener_config_file = (char *) calloc(strlen(config_files_path) + strlen("gener_config_file.xml") + 2, sizeof(char)); // 2 -> one for possible '/' and one terminating
         running_config_file = (char *) calloc(strlen(config_files_path) + strlen("running_config_file.xml") + 2, sizeof(char));
      if (config_files_path[strlen(config_files_path) - 1] == '/') {
         sprintf(gener_config_file, "%sgener_config_file.xml", config_files_path);
         sprintf(running_config_file, "%srunning_config_file.xml", config_files_path);
      } else {
         sprintf(gener_config_file, "%s/gener_config_file.xml", config_files_path);
         sprintf(running_config_file, "%s/running_config_file.xml", config_files_path);
      }
   } else {
         if (check_file_type_perm(DEFAULT_PATH_TO_CONFIGSS, CHECK_DIR, R_OK | W_OK) == -1) {
            VERBOSE(N_STDOUT, "[ERROR] Path for configuration files \"%s\" is not a directory or it has wrong permissions (read and write needed).\n", DEFAULT_PATH_TO_CONFIGSS);
            return -1;
         }
         gener_config_file = (char *) calloc(strlen(DEFAULT_PATH_TO_CONFIGSS) + strlen("gener_config_file.xml") + 2, sizeof(char)); // 2 -> one for possible '/' and one terminating
         running_config_file = (char *) calloc(strlen(DEFAULT_PATH_TO_CONFIGSS) + strlen("running_config_file.xml") + 2, sizeof(char));
      if (DEFAULT_PATH_TO_CONFIGSS[strlen(DEFAULT_PATH_TO_CONFIGSS) - 1] == '/') {
         sprintf(gener_config_file, "%sgener_config_file.xml", DEFAULT_PATH_TO_CONFIGSS);
         sprintf(running_config_file, "%srunning_config_file.xml", DEFAULT_PATH_TO_CONFIGSS);
      } else {
         sprintf(gener_config_file, "%s/gener_config_file.xml", DEFAULT_PATH_TO_CONFIGSS);
         sprintf(running_config_file, "%s/running_config_file.xml", DEFAULT_PATH_TO_CONFIGSS);
      }
   }
   return 0;
}

int supervisor_initialization()
{
   time(&sup_init_time);

   if (init_files() == -1) {
      fprintf(stderr, "[ERROR] Could not create needed files and directories\n");
      return -1;
   }

   // check_permissions_of_all_needed_files!!!

   // Allocate running_modules memory
   running_modules_array_size = 0;
   check_running_modules_allocated_memory();

   // Initialize main mutex
   pthread_mutex_init(&running_modules_lock,NULL);

   // Load startup configuration
   if (netconf_flag == FALSE) {
      VERBOSE(N_STDOUT,"[INIT LOADING CONFIGURATION]\n");
      reload_configuration(RELOAD_INIT_LOAD_CONFIG, NULL);
   }

   // Check and create (if it doesn't exist) directory for all output (started modules and also supervisor's) according to the logs_path
   if (init_sup_logs_dir() != -1) {
      // Create strings with supervisor's output files names and get their file descriptors
      init_sup_logs_files();
      // Append content of tmp log files to already created logs
      append_tmp_logs();
   }

   // Make sup tmp dir in /tmp
   if (mkdir(SUP_TMP_DIR, PERM_LOGSDIR) == -1) {
      if (errno == EACCES) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create tmp dir \"%s\" because of permissions.\n", SUP_TMP_DIR);
      } else if (errno == ENOENT || errno == ENOTDIR) {
         VERBOSE(N_STDOUT, "[ERROR] I/O, could not create tmp dir \"%s\".\n", SUP_TMP_DIR);
      }
   }

   // Create a new thread doing service routine
   VERBOSE(N_STDOUT,"[SERVICE] Starting service thread.\n");
   if (start_service_thread() != 0) {
      service_thread_initialized = FALSE;
   } else {
      service_thread_initialized = TRUE;
   }

   /************ SIGNAL HANDLING *************/
   if (netconf_flag == FALSE) {
      struct sigaction sig_action;
      sig_action.sa_handler = sup_sig_handler;
      sig_action.sa_flags = 0;
      sigemptyset(&sig_action.sa_mask);

      if (sigaction(SIGPIPE,&sig_action,NULL) == -1) {
         VERBOSE(N_STDOUT,"%s [ERROR] Sigaction: signal handler won't catch SIGPIPE !\n", get_formatted_time());
      }
      if (sigaction(SIGINT,&sig_action,NULL) == -1) {
         VERBOSE(N_STDOUT,"%s [ERROR] Sigaction: signal handler won't catch SIGINT !\n", get_formatted_time());
      }
      if (sigaction(SIGTERM,&sig_action,NULL) == -1) {
         VERBOSE(N_STDOUT,"%s [ERROR] Sigaction: signal handler won't catch SIGTERM !\n", get_formatted_time());
      }
      if (sigaction(SIGSEGV,&sig_action,NULL) == -1) {
         VERBOSE(N_STDOUT,"%s [ERROR] Sigaction: signal handler won't catch SIGSEGV !\n", get_formatted_time());
      }
      if (sigaction(SIGQUIT,&sig_action,NULL) == -1) {
         VERBOSE(N_STDOUT,"%s [ERROR] Sigaction: signal handler won't catch SIGQUIT !\n", get_formatted_time());
      }
   }
   /****************************************/

   supervisor_initialized = TRUE;
   if (service_thread_initialized == TRUE) {
      return 0;
   } else {
      return -1;
   }
}

int start_service_thread()
{
   service_stop_all_modules = FALSE;
   service_thread_continue = TRUE;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   return pthread_create(&service_thread_id,  &attr, service_thread_routine, NULL);
}

int parse_prog_args(int *argc, char **argv)
{
   /******/
   static struct option long_options[] = {
      {"daemon", no_argument, 0, 'd'},
      {"config-template", required_argument, 0, 'T'},
      {"configs-path",  required_argument,    0, 'C'},
      {"help", no_argument,           0,  'h' },
      {"verbose",  no_argument,       0,  'v' },
      {"daemon-socket",  required_argument,  0, 's'},
      {"logs-path",  required_argument,  0, 'L'},
      {0, 0, 0, 0}
   };
   /******/

   char c = 0;

   while (1) {
      c = SUP_GETOPT(*argc, argv, "dC:T:hvs:L:", long_options);
      if (c == -1) {
         break;
      }

      switch (c) {
      case ':':
         fprintf(stderr, "Wrong arguments, use \"supervisor -h\" for help.\n");
         return -1;
      case '?':
         fprintf(stderr, "Unknown option, use \"supervisor -h\" for help.\n");
         return -1;
      case 'h':
         printf("Usage: supervisor [-d|--daemon] -T|--config-template=path [-h|--help] [-L|--logs-path] [-s|--daemon-socket=path]\n");
         return -1;
      case 's':
         socket_path = optarg;
         break;
      case 'T':
         NULLP_TEST_AND_FREE(templ_config_file)
         templ_config_file = strdup(optarg);
         break;
      case 'C':
         NULLP_TEST_AND_FREE(config_files_path);
         config_files_path = strdup(optarg);
         break;
      case 'd':
         daemon_flag = TRUE;
         break;
      case 'L':
         NULLP_TEST_AND_FREE(logs_path)
         logs_path = strdup(optarg);
         break;
      }
   }

   if (socket_path == NULL) {
      /* socket_path was not set by user, use default value. */
      socket_path = DEFAULT_DAEMON_SERVER_SOCKET;
   }
   if (templ_config_file == NULL) {
      fprintf(stderr, "Missing required config template (-T|--config-template).\n");
      return -1;
   }
   if (strstr(templ_config_file, ".xml") == NULL) {
      NULLP_TEST_AND_FREE(templ_config_file)
      fprintf(stderr, "File does not have expected .xml extension.\n");
      return -1;
   }

   if (daemon_flag == TRUE) {
      return DAEMON_MODE_CODE;
   } else {
      return INTERACTIVE_MODE_CODE;
   }
}



/*****************************************************************
 * Reload function and functions used by reload *
 *****************************************************************/

int reload_check_supervisor_element(reload_config_vars_t **config_vars)
{
   xmlChar *key = NULL;
   int number = 0;
   int basic_elements[2];
   memset(basic_elements, 0, 2 * sizeof(int));
   uint8_t restarts_elem_idx = 0, logsdir_elem_idx = 1;

   while ((*config_vars)->module_elem != NULL) {
      if ((*config_vars)->module_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "module-restarts") == 0)) {
         basic_elements[restarts_elem_idx]++;
         /* Check the number of found elements module-restarts (at most 1 is allowed) */
         if (basic_elements[restarts_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"module-restarts\" elements in \"supervisor\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            /* The value in module-restarts element must be positive number (including 0) */
            if ((sscanf((const char *) key,"%d",&number) != 1) || (number < 0)) {
               VERBOSE(N_STDOUT, "[ERROR] Value in \"module-restarts\" element must be positive number!\n");
               goto error_label;
            }
         } else {
            /* Empty module-restarts element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"module-restarts\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "logs-directory") == 0)) {
         basic_elements[logsdir_elem_idx]++;
         /* Check the number of found elements logs-directory (at most 1 is allowed) */
         if (basic_elements[logsdir_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"logs-directory\" elements in \"supervisor\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            // TODO check whether the directory can be created or not
         } else {
            /* Empty logs-directory element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"logs-directory\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_elem->type == XML_COMMENT_NODE || (*config_vars)->module_elem->type == XML_TEXT_NODE) {
         // Nothing to do here
      } else {
         /* All other nodes are unexpected and are not allowed */
         VERBOSE(N_STDOUT, "[ERROR] Unexpected node (type: %d, name: %s) in \"supervisor\" element!\n", (*config_vars)->module_elem->type, (char *)(*config_vars)->module_elem->name);
         goto error_label;
      }
      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }
      (*config_vars)->module_elem = (*config_vars)->module_elem->next;
   }

   return 0;

error_label:
   if (key != NULL) {
      xmlFree(key);
      key = NULL;
   }
   return -1;
}

void reload_process_supervisor_element(reload_config_vars_t **config_vars)
{
   xmlChar *key = NULL;
   int x = 0, number = 0;
   char *path_old = NULL;
   char *path_new = NULL;

   while ((*config_vars)->module_elem != NULL) {
      if (!xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "module-restarts")) {
         // Process supervisor's element "module-restarts"
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            x = 0;
            if ((sscanf((const char *) key,"%d",&number) == 1) && (number >= 0)) {
               x = (unsigned int) number;
               max_restarts_per_minute_config = x;
            }
         }
      } else if (!xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "logs-directory")) {
         // Process supervisor's element "logs-directory"
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            if (logs_paths_initialized == FALSE) { // Initial reloading (paths haven't been checked yet) - the logs path in the configuration file has bigger priority than the path from -L parameter
               NULLP_TEST_AND_FREE(logs_path) // Free allocated string from -L parameter (if there was any)
               logs_path = (char *) xmlStrdup(key);
            } else { // Reloading during runtime (supervisor has been initialized, logs paths are already created)
               path_new = get_absolute_file_path((char *) key);
               if (path_new == NULL) { // In case the new path does not exists, use it (if it won't be a valid path for logs - permissions etc., default logs path will be used)
                  NULLP_TEST_AND_FREE(logs_path)
                  logs_path = (char *) xmlStrdup(key);
                  init_sup_logs_dir();
                  init_sup_logs_files();
               } else {
                  path_new = strdup(path_new);
                  path_old = strdup(get_absolute_file_path(logs_path));
                  if (strcmp(path_old, path_new) != 0) { // If it exists and it is not same as the current logs path, use it
                     NULLP_TEST_AND_FREE(logs_path)
                     logs_path = (char *) xmlStrdup(key);
                     init_sup_logs_dir();
                     init_sup_logs_files();
                  }
                  NULLP_TEST_AND_FREE(path_new)
                  NULLP_TEST_AND_FREE(path_old)
               }
            }
         }
      }
      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }
      (*config_vars)->module_elem = (*config_vars)->module_elem->next;
   }
   return;
}

void reload_process_module_atribute(reload_config_vars_t **config_vars, char **module_ifc_atr)
{
   xmlChar *key = NULL;

   key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
   if ((*config_vars)->new_module == FALSE) {
      if (*module_ifc_atr != NULL && key != NULL) {
         if (xmlStrcmp(key, BAD_CAST *module_ifc_atr) != 0) {
            VERBOSE(N_STDOUT, "[WARNING] %s's attribute \"%s\" has been changed (%s -> %s), gonna update it.\n",
               running_modules[(*config_vars)->current_module_idx].module_name, (char *) (*config_vars)->module_atr_elem->name,
               *module_ifc_atr, (char *)key);
            running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
            if (*module_ifc_atr != NULL) {
               free(*module_ifc_atr);
               *module_ifc_atr = NULL;
            }
            *module_ifc_atr = (char *) xmlStrdup(key);
         }
      } else if (*module_ifc_atr == NULL && key == NULL) {
         // new one and old one NULL -> OK
      } else if (*module_ifc_atr == NULL) {
         VERBOSE(N_STDOUT, "[WARNING] %s's attribute \"%s\" should be empty, gonna update it.\n",
               running_modules[(*config_vars)->current_module_idx].module_name, (char *) (*config_vars)->module_atr_elem->name);
         running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
         *module_ifc_atr = (char *) xmlStrdup(key);
      } else if (key == NULL) {
         VERBOSE(N_STDOUT, "[WARNING] %s's attribute \"%s\" shouldn't be empty, gonna update it.\n",
               running_modules[(*config_vars)->current_module_idx].module_name, (char *) (*config_vars)->module_atr_elem->name);
         running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
         if (*module_ifc_atr != NULL) {
            free(*module_ifc_atr);
            *module_ifc_atr = NULL;
         }
      }
   } else {
      if (key == NULL) {
         *module_ifc_atr = NULL;
      } else {
         *module_ifc_atr = (char *) xmlStrdup(key);
      }
   }

   if (key != NULL) {
      xmlFree(key);
      key = NULL;
   }

   return;
}

int reload_check_interface_element(reload_config_vars_t **config_vars)
{
   xmlChar *key = NULL;
   int basic_elements[4];
   memset(basic_elements, 0, 4 * sizeof(int));
   uint8_t note_elem_idx = 0, type_elem_idx = 1, dir_elem_idx = 2, params_elem_idx = 3;

   while ((*config_vars)->ifc_atr_elem != NULL) {
      if ((*config_vars)->ifc_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->ifc_atr_elem->name,BAD_CAST "note") == 0)) {
         basic_elements[note_elem_idx]++;
         /* Check the number of found elements note (at most 1 is allowed) */
         if (basic_elements[note_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"note\" elements in \"interface\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->ifc_atr_elem->xmlChildrenNode, 1);
         if (key == NULL) {
            /* Empty note element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"note\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->ifc_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->ifc_atr_elem->name,BAD_CAST "type") == 0)) {
         basic_elements[type_elem_idx]++;
         /* Check the number of found elements type (at most 1 is allowed) */
         if (basic_elements[type_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"type\" elements in \"interface\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->ifc_atr_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            /* Only "TCP", "UNIXSOCKET", "FILE" or "BLACKHOLE" values in element type are allowed */
            if (xmlStrcmp(key, BAD_CAST "TCP") != 0 && xmlStrcmp(key, BAD_CAST "UNIXSOCKET") != 0 &&
                xmlStrcmp(key, BAD_CAST "FILE") != 0 && xmlStrcmp(key, BAD_CAST "BLACKHOLE") != 0) {
               VERBOSE(N_STDOUT, "[ERROR] Expected one of {TCP,UNIXSOCKET,FILE,BLACKHOLE} values in \"type\" element!\n");
               goto error_label;
            }
         } else {
            /* Empty type element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"type\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->ifc_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->ifc_atr_elem->name,BAD_CAST "direction") == 0)) {
         basic_elements[dir_elem_idx]++;
         /* Check the number of found elements direction (at most 1 is allowed) */
         if (basic_elements[dir_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"direction\" elements in \"interface\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->ifc_atr_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            /* Only "IN" or "OUT" values in element type are allowed */
            if (xmlStrcmp(key, BAD_CAST "IN") != 0 && xmlStrcmp(key, BAD_CAST "OUT") != 0) {
               VERBOSE(N_STDOUT, "[ERROR] Expected one of {IN,OUT} values in \"direction\" element!\n");
               goto error_label;
            }
         } else {
            /* Empty direction element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"direction\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->ifc_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->ifc_atr_elem->name,BAD_CAST "params") == 0)) {
         basic_elements[params_elem_idx]++;
         /* Check the number of found elements params (at most 1 is allowed) */
         if (basic_elements[params_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"params\" elements in \"interface\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->ifc_atr_elem->xmlChildrenNode, 1);
         if (key == NULL) {
            /* Empty params element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"params\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->ifc_atr_elem->type == XML_COMMENT_NODE || (*config_vars)->ifc_atr_elem->type == XML_TEXT_NODE) {
         // Nothing to do here
      } else {
         /* All other nodes are unexpected and are not allowed */
         VERBOSE(N_STDOUT, "[ERROR] Unexpected node (type: %d, name: %s) in \"interface\" element!\n", (*config_vars)->ifc_atr_elem->type, (char *)(*config_vars)->ifc_atr_elem->name);
         goto error_label;
      }
      (*config_vars)->ifc_atr_elem=(*config_vars)->ifc_atr_elem->next;
      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }
   }

   return 0;

error_label:
   if (key != NULL) {
      xmlFree(key);
      key = NULL;
   }
   return -1;
}

int reload_process_module_interface_atribute(reload_config_vars_t **config_vars, char **module_ifc_atr)
{
   int str_len = 0;
   xmlChar *key = NULL;

   key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->ifc_atr_elem->xmlChildrenNode, 1);
   if ((*config_vars)->module_ifc_insert == FALSE) {
      if (*module_ifc_atr != NULL && key != NULL) {
         if (xmlStrcmp(key, BAD_CAST *module_ifc_atr) != 0) {
            VERBOSE(N_STDOUT, "[WARNING] %s's interface attribute \"%s\" has been changed (%s -> %s), gonna update module's interfaces.\n",
               running_modules[(*config_vars)->current_module_idx].module_name, (*config_vars)->ifc_atr_elem->name,
               *module_ifc_atr, (char *)key);
            running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
            free_module_interfaces_on_index((*config_vars)->current_module_idx);
            (*config_vars)->ifc_elem = (*config_vars)->module_atr_elem->xmlChildrenNode;
            running_modules[(*config_vars)->current_module_idx].module_ifces_cnt = -1;
            running_modules[(*config_vars)->current_module_idx].module_num_out_ifc = 0;
            running_modules[(*config_vars)->current_module_idx].module_num_in_ifc = 0;
            (*config_vars)->module_ifc_insert = TRUE;

            if (key != NULL) {
               xmlFree(key);
               key = NULL;
            }
            return -1;
         }

         if (key != NULL) {
            xmlFree(key);
            key = NULL;
         }
      } else if (*module_ifc_atr == NULL && key == NULL) {
         // new one and old one NULL -> OK
      } else {
         if (key == NULL) {
            VERBOSE(N_STDOUT, "[WARNING] %s's interface attribute \"%s\" shouldn't be empty, gonna update module's interfaces.\n",
               running_modules[(*config_vars)->current_module_idx].module_name, (*config_vars)->ifc_atr_elem->name);
         } else {
            VERBOSE(N_STDOUT, "[WARNING] %s's interface attribute \"%s\" should be empty, gonna update module's interfaces.\n",
               running_modules[(*config_vars)->current_module_idx].module_name, (*config_vars)->ifc_atr_elem->name);
         }
         running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
         free_module_interfaces_on_index((*config_vars)->current_module_idx);
         (*config_vars)->ifc_elem = (*config_vars)->module_atr_elem->xmlChildrenNode;
         running_modules[(*config_vars)->current_module_idx].module_ifces_cnt = -1;
         running_modules[(*config_vars)->current_module_idx].module_num_out_ifc = 0;
         running_modules[(*config_vars)->current_module_idx].module_num_in_ifc = 0;
         (*config_vars)->module_ifc_insert = TRUE;

         if (key != NULL) {
            xmlFree(key);
            key = NULL;
         }
         return -1;
      }
   } else {
      if (key == NULL) {
         *module_ifc_atr = NULL;
      } else {
         str_len = strlen((char *) key);
         *module_ifc_atr = (char *) calloc(str_len+1, sizeof(char));
         strncpy(*module_ifc_atr , (char *) key, str_len+1);

         if (key != NULL) {
            xmlFree(key);
            key = NULL;
         }
      }
   }

   return 0;
}

void reload_check_modules_interfaces_count(reload_config_vars_t  **config_vars)
{
   int original_module_ifc_cnt = running_modules[(*config_vars)->current_module_idx].module_ifces_cnt;
   int new_module_ifc_cnt = 0;

   while ((*config_vars)->ifc_elem != NULL) {
      if (!xmlStrcmp((*config_vars)->ifc_elem->name,BAD_CAST "interface")) {
         new_module_ifc_cnt++;
      }
      (*config_vars)->ifc_elem = (*config_vars)->ifc_elem->next;
   }

   (*config_vars)->ifc_elem = (*config_vars)->module_atr_elem->xmlChildrenNode;

   if (original_module_ifc_cnt != new_module_ifc_cnt) {
      running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
      free_module_interfaces_on_index((*config_vars)->current_module_idx);
      running_modules[(*config_vars)->current_module_idx].module_ifces_cnt = 0;
      running_modules[(*config_vars)->current_module_idx].module_num_out_ifc = 0;
      running_modules[(*config_vars)->current_module_idx].module_num_in_ifc = 0;
      (*config_vars)->module_ifc_insert = TRUE;
      VERBOSE(N_STDOUT,"[WARNING] Reloading module \"%s\" - original interface cnt:%d, actual interface cnt:%d -> gonna update module's interfaces.\n",
                                             running_modules[(*config_vars)->current_module_idx].module_name, original_module_ifc_cnt, new_module_ifc_cnt);
   }

   return;
}

int reload_check_module_element(reload_config_vars_t **config_vars, str_lst_t **first_module_name, str_lst_t **last_module_name)
{
   int number = 0;
   str_lst_t *ptr1 = NULL;
   char *new_module_name = NULL;
   xmlChar *key = NULL;
   int basic_elements[6], name_elem_idx = 0, path_elem_idx = 1, trapifc_elem_idx = 2, enabled_elem_idx = 3, restarts_elem_idx = 4, params_elem_idx = 5;
   memset(basic_elements, 0, 6 * sizeof(int));

   while ((*config_vars)->module_atr_elem != NULL) {
      if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name, BAD_CAST "name") == 0)) {
         basic_elements[name_elem_idx]++;
         /* Check the number of found elements name (at most 1 is allowed) */
         if (basic_elements[name_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"name\" elements in \"module\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            new_module_name = strdup((char *) key);
            /* Add the module name to linked list */
            if ((*first_module_name) == NULL) {
               (*first_module_name) = (str_lst_t *) calloc(1, sizeof(str_lst_t));
               (*first_module_name)->str = new_module_name;
               (*first_module_name)->next = NULL;
               (*last_module_name) = (*first_module_name);
            } else {
               ptr1 = (*first_module_name);
               while (ptr1 != NULL) {
                  // Check whether the module name is duplicated
                  if ((strlen(new_module_name) == strlen(ptr1->str)) && strcmp(new_module_name, ptr1->str) == 0) {
                     VERBOSE(N_STDOUT, "[ERROR] Duplicated module name \"%s\"\n", new_module_name);
                     NULLP_TEST_AND_FREE(new_module_name)
                     goto error_label;
                  }
                  ptr1 = ptr1->next;
               }
               ptr1 = (str_lst_t *) calloc(1, sizeof(str_lst_t));
               ptr1->str = new_module_name;
               ptr1->next = NULL;
               (*last_module_name)->next = ptr1;
               (*last_module_name) = ptr1;
            }
         } else {
            /* Empty element name is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"name\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name, BAD_CAST "enabled") == 0)) {
         basic_elements[enabled_elem_idx]++;
         /* Check the number of found elements enabled (at most 1 is allowed) */
         if (basic_elements[enabled_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"enabled\" elements in \"module\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            /* Only "true" or "false" values in element enabled are allowed */
            if (xmlStrcmp(key, BAD_CAST "true") != 0 && xmlStrcmp(key, BAD_CAST "false") != 0) {
               VERBOSE(N_STDOUT, "[ERROR] Expected one of {true,false} values in \"enabled\" element!\n");
               goto error_label;
            }
         } else {
            /* Empty element enabled is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"enabled\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name,BAD_CAST "path") == 0)) {
         basic_elements[path_elem_idx]++;
         /* Check the number of found elements path (at most 1 is allowed) */
         if (basic_elements[path_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"path\" elements in \"module\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            // TODO check whether the directory can be created or not
         } else {
            /* Empty element path is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"path\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name,BAD_CAST "trapinterfaces") == 0)) {
         basic_elements[trapifc_elem_idx]++;
         /* Check the number of found elements trapinterfaces (at most 1 is allowed) */
         if (basic_elements[trapifc_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"trapinterfaces\" elements in \"module\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name, BAD_CAST "module-restarts") == 0)) {
         basic_elements[restarts_elem_idx]++;
         /* Check the number of found elements module-restarts (at most 1 is allowed) */
         if (basic_elements[restarts_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"module-restarts\" elements in \"module\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            /* The value in module-restarts element must be positive number (including 0) */
            if ((sscanf((const char *) key,"%d",&number) != 1) || (number < 0)) {
               VERBOSE(N_STDOUT, "[ERROR] Value in \"module-restarts\" element must be positive number!\n");
               goto error_label;
            }
         } else {
            /* Empty module-restarts element is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"module-restarts\" element!\n");
            goto error_label;
         }

      } else if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name,BAD_CAST "params") == 0)) {
         basic_elements[params_elem_idx]++;
         /* Check the number of found elements params (at most 1 is allowed) */
         if (basic_elements[params_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"params\" elements in \"module\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key == NULL) {
            /* Empty element params is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"params\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_atr_elem->type == XML_COMMENT_NODE || (*config_vars)->module_atr_elem->type == XML_TEXT_NODE) {
         // Nothing to do here
      } else {
         /* All other nodes are unexpected and are not allowed */
         VERBOSE(N_STDOUT, "[ERROR] Unexpected node (type: %d, name: %s) in \"module\" element!\n", (*config_vars)->module_atr_elem->type, (char *)(*config_vars)->module_atr_elem->name);
         goto error_label;
      }
      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }
      (*config_vars)->module_atr_elem = (*config_vars)->module_atr_elem->next;
   }

   /* Check whether the mandatory elements were found */
   if (basic_elements[name_elem_idx] == 0) {
      VERBOSE(N_STDOUT, "[ERROR] Missing \"name\" element in \"module\" element!\n");
      goto error_label;
   } else if (basic_elements[path_elem_idx] == 0) {
      VERBOSE(N_STDOUT, "[ERROR] Missing \"path\" element in \"module\" element!\n");
      goto error_label;
   } else if (basic_elements[enabled_elem_idx] == 0) {
      VERBOSE(N_STDOUT, "[ERROR] Missing \"enabled\" element in \"module\" element!\n");
      goto error_label;
   }

   return 0;

error_label:
   if (key != NULL) {
      xmlFree(key);
      key = NULL;
   }
   return -1;
}

int reload_find_and_check_module_basic_elements(reload_config_vars_t **config_vars)
{
   int move_to_next_module = FALSE;
   int ret_val = 0;
   uint8_t last_module = FALSE, unique_name = TRUE;
   xmlChar *key = NULL;
   int basic_elements[3], name_elem_idx = 0, path_elem_idx = 1, trapifc_elem_idx = 2;
   memset(basic_elements, 0, 3*sizeof(int));

   while ((*config_vars)->module_atr_elem != NULL) {
      if ((!xmlStrcmp((*config_vars)->module_atr_elem->name, BAD_CAST "name"))) {
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key == NULL) {
            basic_elements[name_elem_idx] = -1;
            move_to_next_module = TRUE;
         } else {
            basic_elements[name_elem_idx]++;
            if (basic_elements[name_elem_idx] == 1) {
               ret_val = find_loaded_module((char *) key);
               if (ret_val == -1) { // Module with this name was not found - gonna insert a new module
                  (*config_vars)->current_module_idx = loaded_modules_cnt;
                  (*config_vars)->new_module = TRUE;
                  (*config_vars)->module_ifc_insert = TRUE;
               } else { // Found already loaded module with same name -> check it's values
                  if (running_modules[ret_val].module_checked_by_reload == TRUE) {
                     move_to_next_module = TRUE;
                     unique_name = FALSE;
                  }
                  (*config_vars)->current_module_idx = ret_val;
                  (*config_vars)->new_module = FALSE;
                  (*config_vars)->module_ifc_insert = FALSE;
               }
            } else {
               move_to_next_module = TRUE;
            }
         }
      } else if ((!xmlStrcmp((*config_vars)->module_atr_elem->name,BAD_CAST "path"))) {
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
         if (key == NULL) {
            basic_elements[path_elem_idx] = -1;
            move_to_next_module = TRUE;
         } else {
            basic_elements[path_elem_idx]++;
            if (basic_elements[path_elem_idx] > 1) {
               move_to_next_module = TRUE;
            }
         }
      } else if ((!xmlStrcmp((*config_vars)->module_atr_elem->name,BAD_CAST "trapinterfaces"))) {
         basic_elements[trapifc_elem_idx]++;
         if (basic_elements[trapifc_elem_idx] > 1) {
            move_to_next_module = TRUE;
         }
      }

      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }

      // If there is no more children of module element and path or name elements were not found, move to next module
      if ((*config_vars)->module_atr_elem->next == NULL && (basic_elements[name_elem_idx] == 0 || basic_elements[path_elem_idx] == 0)) {
         move_to_next_module = TRUE;
      }

      if (move_to_next_module) {
         if (unique_name == FALSE) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading module \"%s\" - module with the same name was already found in the configuration file (module name must be unique!) -> skipping this module.\n",
                                      running_modules[ret_val].module_name);
         } else if (basic_elements[name_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - found more \"name\" elements in module -> moving to next module.\n");
         } else if (basic_elements[name_elem_idx] == 0) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - didn't find \"name\" element in module -> moving to next module.\n");
         } else if (basic_elements[name_elem_idx] == -1) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - found empty \"name\" element in module -> moving to next module.\n");
         } else if (basic_elements[path_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - found more \"path\" elements in module -> moving to next module.\n");
         } else if (basic_elements[path_elem_idx] == 0) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - didn't find \"path\" element in module -> moving to next module.\n");
         } else if (basic_elements[path_elem_idx] == -1) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - found empty \"path\" element in module -> moving to next module.\n");
         } else if (basic_elements[trapifc_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[WARNING] Reloading error - found more \"trapinterfaces\" elements in module -> moving to next module.\n");
         }

         (*config_vars)->module_elem = (*config_vars)->module_elem->next;
         if ((*config_vars)->module_elem == NULL) {
            last_module = TRUE;
            break;
         } else {
            (*config_vars)->module_elem = (*config_vars)->module_elem->next;
            if ((*config_vars)->module_elem == NULL) {
               last_module = TRUE;
               break;
            }
            (*config_vars)->current_module_idx = -1;
            memset(basic_elements,0,3*sizeof(int));
            (*config_vars)->module_atr_elem = (*config_vars)->module_elem->xmlChildrenNode;
         }
         move_to_next_module = FALSE;
         unique_name = TRUE;
         continue;
      }

      (*config_vars)->module_atr_elem = (*config_vars)->module_atr_elem->next;
   }

   if (last_module) {
      return -1;
   }

   if ((basic_elements[name_elem_idx] != 1) || (basic_elements[path_elem_idx] != 1) || (basic_elements[trapifc_elem_idx] > 1)) {
      return -1;
   }

   // If the module was already in configuration and had some interfaces, check if trapinterfaces element was found; if not, delete the interfaces.
   if ((*config_vars)->new_module == FALSE && basic_elements[trapifc_elem_idx] == 0 && running_modules[(*config_vars)->current_module_idx].module_ifces_cnt > 0) {
      VERBOSE(N_STDOUT,"[WARNING] Reloading module \"%s\" - original interface cnt:%d, but trapinterfaces element was not found -> gonna remove module's interfaces.\n",
                                             running_modules[(*config_vars)->current_module_idx].module_name, running_modules[(*config_vars)->current_module_idx].module_ifces_cnt);
      running_modules[(*config_vars)->current_module_idx].module_modified_by_reload = TRUE;
      free_module_interfaces_on_index((*config_vars)->current_module_idx);
      running_modules[(*config_vars)->current_module_idx].module_ifces_cnt = 0;
      running_modules[(*config_vars)->current_module_idx].module_num_out_ifc = 0;
      running_modules[(*config_vars)->current_module_idx].module_num_in_ifc = 0;
      (*config_vars)->module_ifc_insert = TRUE;
   }

   return 0;
}

int reload_check_modules_element(reload_config_vars_t **config_vars, str_lst_t **first_profile_name, str_lst_t **last_profile_name)
{
   str_lst_t *ptr1 = NULL;
   char *new_profile_name = NULL;
   xmlChar *key = NULL;
   int basic_elements[2], name_elem_idx = 0, enabled_elem_idx = 1;
   memset(basic_elements, 0, 2*sizeof(int));

   while ((*config_vars)->module_elem != NULL) {
      if ((*config_vars)->module_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "name") == 0)) {
         basic_elements[name_elem_idx]++;
         /* Check the number of found elements name (at most 1 is allowed) */
         if (basic_elements[name_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"name\" elements in \"modules\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            new_profile_name = strdup((char *) key);
            /* Add the profile name to linked list */
            if ((*first_profile_name) == NULL) {
               (*first_profile_name) = (str_lst_t *) calloc(1, sizeof(str_lst_t));
               (*first_profile_name)->str = new_profile_name;
               (*first_profile_name)->next = NULL;
               (*last_profile_name) = (*first_profile_name);
            } else {
               ptr1 = (*first_profile_name);
               while (ptr1 != NULL) {
                  // Check whether the profile name is duplicated
                  if ((strlen(new_profile_name) == strlen(ptr1->str)) && strcmp(new_profile_name, ptr1->str) == 0) {
                     VERBOSE(N_STDOUT, "[ERROR] Duplicated profile name \"%s\"\n", new_profile_name);
                     NULLP_TEST_AND_FREE(new_profile_name)
                     goto error_label;
                  }
                  ptr1 = ptr1->next;
               }
               ptr1 = (str_lst_t *) calloc(1, sizeof(str_lst_t));
               ptr1->str = new_profile_name;
               ptr1->next = NULL;
               (*last_profile_name)->next = ptr1;
               (*last_profile_name) = ptr1;
            }
         } else {
            /* Empty element name is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"name\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "enabled") == 0)) {
         basic_elements[enabled_elem_idx]++;
         /* Check the number of found elements enabled (at most 1 is allowed) */
         if (basic_elements[enabled_elem_idx] > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"enabled\" elements in \"modules\" element!\n");
            goto error_label;
         }
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            /* Only "true" or "false" values in element enabled are allowed */
            if (xmlStrcmp(key, BAD_CAST "true") != 0 && xmlStrcmp(key, BAD_CAST "false") != 0) {
               VERBOSE(N_STDOUT, "[ERROR] Expected one of {true,false} values in \"enabled\" element!\n");
               goto error_label;
            }
         } else {
            /* Empty element enabled is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty value in \"enabled\" element!\n");
            goto error_label;
         }
      } else if ((*config_vars)->module_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "module") == 0)) {
         // Nothing to do here
      } else if ((*config_vars)->module_elem->type == XML_COMMENT_NODE || (*config_vars)->module_elem->type == XML_TEXT_NODE) {
         // Nothing to do here
      } else {
         /* All other nodes are unexpected and are not allowed */
         VERBOSE(N_STDOUT, "[ERROR] Unexpected node (type: %d, name: %s) in \"modules\" element!\n", (*config_vars)->module_elem->type, (char *)(*config_vars)->module_elem->name);
         goto error_label;
      }
      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }
      (*config_vars)->module_elem = (*config_vars)->module_elem->next;
   }

   return 0;

error_label:
   if (key != NULL) {
      xmlFree(key);
      key = NULL;
   }
   return -1;
}

int reload_find_and_check_modules_profile_basic_elements(reload_config_vars_t **config_vars)
{
   int new_profile_enabled = FALSE;
   char *new_profile_name = NULL;
   xmlChar *key = NULL;
   int basic_elements[2], name_elem_idx = 0, enabled_elem_idx = 1;
   memset(basic_elements, 0, 2*sizeof(int));

   while ((*config_vars)->module_elem != NULL) {
      if (!xmlStrcmp((*config_vars)->module_elem->name,BAD_CAST "name")) {
         // Process modules element "name"
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            basic_elements[name_elem_idx]++;
            if (basic_elements[name_elem_idx] == 1) {
               new_profile_name = (char *) xmlStrdup(key);
            } else { // Found more than one modules element "name" -> invalid profile
               break;
            }
         } else { // Modules element "name" is empty -> invalid profile
            basic_elements[name_elem_idx] = -1;
            break;
         }
      } else if (!xmlStrcmp((*config_vars)->module_elem->name,BAD_CAST "enabled")) {
         // Process modules element "enabled"
         key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_elem->xmlChildrenNode, 1);
         if (key != NULL) {
            basic_elements[enabled_elem_idx]++;
            if (basic_elements[enabled_elem_idx] == 1) {
               if (xmlStrcmp(key, BAD_CAST "true") == 0) {
                  new_profile_enabled = TRUE;
               } else {
                  new_profile_enabled = FALSE;
               }
            } else { // Found more than one modules element "enabled" -> invalid profile
               break;
            }
         } else { // Modules element "enabled" is empty -> invalid profile
            basic_elements[enabled_elem_idx] = -1;
            break;
         }
      }

      if (key != NULL) {
         xmlFree(key);
         key = NULL;
      }
      (*config_vars)->module_elem = (*config_vars)->module_elem->next;
   }

   if (key != NULL) {
      xmlFree(key);
      key = NULL;
   }

   if (basic_elements[name_elem_idx] != 1 || basic_elements[enabled_elem_idx] != 1) {
      // Invalid profile
      if (basic_elements[name_elem_idx] > 1) { // Found more "name" elements
         VERBOSE(N_STDOUT, "[WARNING] Reloading error - found more \"name\" elements in modules profile.\n");
      } else if (basic_elements[name_elem_idx] == -1) { // Found empty "name" element
         VERBOSE(N_STDOUT, "[WARNING] Reloading error - found empty \"name\" element in modules profile.\n");
      } else if (basic_elements[enabled_elem_idx] > 1) { // Found more "enabled" elements
         VERBOSE(N_STDOUT, "[WARNING] Reloading error - found more \"enabled\" elements in modules profile.\n");
      } else if (basic_elements[enabled_elem_idx] == -1) { // Found empty "enabled" element
         VERBOSE(N_STDOUT, "[WARNING] Reloading error - found empty \"enabled\" element in modules profile.\n");
      }
      NULLP_TEST_AND_FREE(new_profile_name)
      return -1;
   } else { // Valid profile -> allocate it
      if (first_profile_ptr == NULL) {
         first_profile_ptr = (modules_profile_t *) calloc(1, sizeof(modules_profile_t));
         first_profile_ptr->profile_name = new_profile_name;
         first_profile_ptr->profile_enabled = new_profile_enabled;
         first_profile_ptr->next = NULL;
         actual_profile_ptr = first_profile_ptr;
      } else {
         modules_profile_t * ptr = (modules_profile_t *) calloc(1, sizeof(modules_profile_t));
         ptr->profile_name = new_profile_name;
         ptr->profile_enabled = new_profile_enabled;
         ptr->next = NULL;
         actual_profile_ptr->next = ptr;
         actual_profile_ptr = ptr;
      }
   }
   return 0;
}

void reload_count_module_interfaces(reload_config_vars_t **config_vars)
{
   int x = 0;

   running_modules[(*config_vars)->current_module_idx].module_num_in_ifc = 0;
   running_modules[(*config_vars)->current_module_idx].module_num_out_ifc = 0;

   for (x=0; x<running_modules[(*config_vars)->current_module_idx].module_ifces_cnt; x++) {
      if (running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_direction != NULL) {
         if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_direction, "IN", 2) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_num_in_ifc++;
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_direction = IN_MODULE_IFC_DIRECTION;
         } else if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_direction, "OUT", 3) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_num_out_ifc++;
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_direction = OUT_MODULE_IFC_DIRECTION;
         } else if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_direction, "SERVICE", 7) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_direction = SERVICE_MODULE_IFC_DIRECTION;
         } else {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_direction = INVALID_MODULE_IFC_ATTR;
         }
      } else {
         running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_direction = INVALID_MODULE_IFC_ATTR;
      }

      if (running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_type != NULL) {
         if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_type, "TCP", 3) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = TCP_MODULE_IFC_TYPE;
         } else if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_type, "UNIXSOCKET", 10) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = UNIXSOCKET_MODULE_IFC_TYPE;
         } else if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_type, "FILE", 4) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = FILE_MODULE_IFC_TYPE;
         } else if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_type, "SERVICE", 7) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = SERVICE_MODULE_IFC_TYPE;
         } else if (strncmp(running_modules[(*config_vars)->current_module_idx].module_ifces[x].ifc_type, "BLACKHOLE", 9) == 0) {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = BLACKHOLE_MODULE_IFC_TYPE;
         } else {
            running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = INVALID_MODULE_IFC_ATTR;
         }
      } else {
         running_modules[(*config_vars)->current_module_idx].module_ifces[x].int_ifc_type = INVALID_MODULE_IFC_ATTR;
      }
   }
   return;
}

void reload_check_module_allocated_interfaces(const int running_module_idx, const int ifc_cnt)
{
   int origin_size = 0;

   if (running_modules[running_module_idx].module_ifces_array_size == 0) {
      running_modules[running_module_idx].module_ifces = (interface_t *) calloc(IFCES_ARRAY_START_SIZE, sizeof(interface_t));
      running_modules[running_module_idx].module_ifces_array_size = IFCES_ARRAY_START_SIZE;
      running_modules[running_module_idx].module_ifces_cnt = 0;
   } else if (ifc_cnt == running_modules[running_module_idx].module_ifces_array_size) {
      origin_size = running_modules[running_module_idx].module_ifces_array_size;
      running_modules[running_module_idx].module_ifces_array_size += running_modules[running_module_idx].module_ifces_array_size/2;
      running_modules[running_module_idx].module_ifces = (interface_t *) realloc (running_modules[running_module_idx].module_ifces, (running_modules[running_module_idx].module_ifces_array_size) * sizeof(interface_t));
      memset(running_modules[running_module_idx].module_ifces + origin_size,0,(origin_size/2)*sizeof(interface_t));
   }
}

void check_running_modules_allocated_memory()
{
   int origin_size = 0, x = 0;

   if (running_modules_array_size == 0) {
      loaded_modules_cnt = 0;
      running_modules_array_size = RUNNING_MODULES_ARRAY_START_SIZE;
      running_modules = (running_module_t *) calloc(running_modules_array_size,sizeof(running_module_t));
      for (x=0; x<running_modules_array_size; x++) {
         running_modules[x].module_ifces = (interface_t *) calloc(IFCES_ARRAY_START_SIZE, sizeof(interface_t));
         running_modules[x].module_running = FALSE;
         running_modules[x].module_ifces_array_size = IFCES_ARRAY_START_SIZE;
         running_modules[x].module_ifces_cnt = 0;
      }
   } else if (loaded_modules_cnt == running_modules_array_size) {
      VERBOSE(N_STDOUT, "[WARNING] Reload - reallocating running_modules memory.\n");
      origin_size = running_modules_array_size;
      running_modules_array_size += running_modules_array_size/2;
      running_modules = (running_module_t * ) realloc (running_modules, (running_modules_array_size)*sizeof(running_module_t));
      memset(running_modules + origin_size,0,(origin_size/2)*sizeof(running_module_t));

      for (x=loaded_modules_cnt; x<running_modules_array_size; x++) {
         running_modules[x].module_ifces = (interface_t *) calloc(IFCES_ARRAY_START_SIZE, sizeof(interface_t));
         running_modules[x].module_running = FALSE;
         running_modules[x].module_ifces_array_size = IFCES_ARRAY_START_SIZE;
         running_modules[x].module_ifces_cnt = 0;
      }
   }
}

void reload_resolve_module_enabled(reload_config_vars_t **config_vars, const int modules_got_profile)
{
   xmlChar *key = NULL;
   int config_module_enabled = FALSE;
   int profile_and_module_enabled_anded = FALSE;

   key = xmlNodeListGetString((*config_vars)->doc_tree_ptr, (*config_vars)->module_atr_elem->xmlChildrenNode, 1);
   if (key == NULL) {
      running_modules[(*config_vars)->current_module_idx].module_enabled = FALSE;
      return;
   } else {
      if (xmlStrncmp(key, BAD_CAST "true", xmlStrlen(key)) == 0) {
         config_module_enabled = TRUE;
      } else if (xmlStrncmp(key, BAD_CAST "false", xmlStrlen(key)) == 0) {
         config_module_enabled = FALSE;
      }
      xmlFree(key);
      key = NULL;
   }

   if (modules_got_profile == TRUE) {
      profile_and_module_enabled_anded = actual_profile_ptr->profile_enabled & config_module_enabled;
      if (profile_and_module_enabled_anded == TRUE && running_modules[(*config_vars)->current_module_idx].module_enabled == FALSE) {
         running_modules[(*config_vars)->current_module_idx].module_restart_cnt = -1;
      }
      // If current module is new in configuration, just save the anded enabled flag and return
      if ((*config_vars)->new_module == TRUE) {
         running_modules[(*config_vars)->current_module_idx].module_enabled = profile_and_module_enabled_anded;
      } else if (profile_and_module_enabled_anded != running_modules[(*config_vars)->current_module_idx].module_enabled) {
         VERBOSE(N_STDOUT, "[WARNING] %s enabled flag has been modified: %s -> %s.\n",
            running_modules[(*config_vars)->current_module_idx].module_name,
            (running_modules[(*config_vars)->current_module_idx].module_enabled == TRUE ? "enabled" : "disabled"),
            (profile_and_module_enabled_anded == TRUE ? "enabled" : "disabled"));
         running_modules[(*config_vars)->current_module_idx].module_enabled = profile_and_module_enabled_anded;
      }
   } else {
      if (config_module_enabled == TRUE && running_modules[(*config_vars)->current_module_idx].module_enabled == FALSE) {
         running_modules[(*config_vars)->current_module_idx].module_restart_cnt = -1;
      }
      // If current module is new in configuration, just save the enabled flag and return
      if ((*config_vars)->new_module == TRUE) {
         running_modules[(*config_vars)->current_module_idx].module_enabled = config_module_enabled;
      } else if (config_module_enabled != running_modules[(*config_vars)->current_module_idx].module_enabled) {
         VERBOSE(N_STDOUT, "[WARNING] %s enabled flag has been modified: %s -> %s.\n",
            running_modules[(*config_vars)->current_module_idx].module_name,
            (running_modules[(*config_vars)->current_module_idx].module_enabled == TRUE ? "enabled" : "disabled"),
            (config_module_enabled == TRUE ? "enabled" : "disabled"));
         running_modules[(*config_vars)->current_module_idx].module_enabled = config_module_enabled;
      }
   }
   return;
}

int validate_configuration(reload_config_vars_t **config_vars)
{
   int ret_val = 0;
   uint8_t supervisor_elem_cnt = 0;
   str_lst_t *first_module_name = NULL, *last_module_name = NULL;
   str_lst_t *first_profile_name = NULL, *last_profile_name = NULL;
   str_lst_t *ptr1 = NULL;

   VERBOSE(N_STDOUT, "- - -\n[RELOAD] Validating the configuration file...\n");

   /* Basic tests of the document */
   if ((*config_vars)->root_node == NULL) {
      VERBOSE(N_STDOUT,"[ERROR] Empty document.\n");
      ret_val = -1;
      goto end_label;
   } else if (xmlStrcmp((*config_vars)->root_node->name, BAD_CAST "nemea-supervisor")) {
      VERBOSE(N_STDOUT,"[ERROR] Document of the wrong type, missing root element \"nemea-supervisor\".\n");
      ret_val = -1;
      goto end_label;
   } else if ((*config_vars)->root_node->xmlChildrenNode == NULL) {
      VERBOSE(N_STDOUT,"[ERROR] There is no child element of the root element \"nemea-supervisor\".\n");
      ret_val = -1;
      goto end_label;
   }

   (*config_vars)->current_node = (*config_vars)->root_node->xmlChildrenNode;

   while ((*config_vars)->current_node != NULL) {
      if ((*config_vars)->current_node->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->current_node->name, BAD_CAST "supervisor") == 0)) {
         supervisor_elem_cnt++;
         /* Check the number of found elements supervisor (at most 1 is allowed) */
         if (supervisor_elem_cnt > 1) {
            VERBOSE(N_STDOUT, "[ERROR] Too much \"supervisor\" elements!\n");
            ret_val = -1;
            goto end_label;
         }
         (*config_vars)->module_elem = (*config_vars)->current_node->xmlChildrenNode;
         if ((*config_vars)->module_elem == NULL) {
            /* Empty element supervisor is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty element \"supervisor\".\n");
            ret_val = -1;
            goto end_label;
         }
         if (reload_check_supervisor_element(config_vars) == -1) {
            ret_val = -1;
            goto end_label;
         }
      } else if ((*config_vars)->current_node->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->current_node->name, BAD_CAST "modules") == 0)) {
         (*config_vars)->module_elem = (*config_vars)->current_node->xmlChildrenNode;
         if ((*config_vars)->module_elem == NULL) {
            /* Empty element modules is not allowed */
            VERBOSE(N_STDOUT, "[ERROR] Empty element \"modules\".\n");
            ret_val = -1;
            goto end_label;
         }
         (*config_vars)->module_atr_elem = NULL, (*config_vars)->ifc_elem = NULL, (*config_vars)->ifc_atr_elem = NULL;
         if (reload_check_modules_element(config_vars, &first_profile_name, &last_profile_name) == -1) {
            ret_val = -1;
            goto end_label;
         }
         (*config_vars)->module_elem = (*config_vars)->current_node->xmlChildrenNode;

         while ((*config_vars)->module_elem != NULL) {
            if ((*config_vars)->module_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_elem->name, BAD_CAST "module") == 0)) {
               (*config_vars)->module_atr_elem = (*config_vars)->module_elem->xmlChildrenNode;
               if ((*config_vars)->module_atr_elem == NULL) {
                  /* Empty element module is not allowed */
                  VERBOSE(N_STDOUT, "[ERROR] Empty element \"module\".\n");
                  ret_val = -1;
                  goto end_label;
               }
               if (reload_check_module_element(config_vars, &first_module_name, &last_module_name) == -1) {
                  ret_val = -1;
                  goto end_label;
               }

               (*config_vars)->module_atr_elem = (*config_vars)->module_elem->xmlChildrenNode;

               while ((*config_vars)->module_atr_elem != NULL) {
                  if ((*config_vars)->module_atr_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->module_atr_elem->name,BAD_CAST "trapinterfaces") == 0)) {
                     (*config_vars)->ifc_elem = (*config_vars)->module_atr_elem->xmlChildrenNode;

                     while ((*config_vars)->ifc_elem != NULL) {
                        if ((*config_vars)->ifc_elem->type == XML_ELEMENT_NODE && (xmlStrcmp((*config_vars)->ifc_elem->name,BAD_CAST "interface") == 0)) {
                           (*config_vars)->ifc_atr_elem = (*config_vars)->ifc_elem->xmlChildrenNode;

                           if ((*config_vars)->ifc_atr_elem == NULL) {
                              /* Empty element interface is not allowed */
                              VERBOSE(N_STDOUT, "[ERROR] Empty element \"interface\".\n");
                              ret_val = -1;
                              goto end_label;
                           }
                           if (reload_check_interface_element(config_vars) == -1) {
                              ret_val = -1;
                              goto end_label;
                           }
                        } else if ((*config_vars)->ifc_elem->type == XML_COMMENT_NODE || (*config_vars)->ifc_elem->type == XML_TEXT_NODE) {
                           // Nothing to do here
                        } else {
                           /* All other nodes are unexpected and are not allowed */
                           VERBOSE(N_STDOUT, "[ERROR] Unexpected node (type: %d, name: %s) in \"trapinterfaces\" element!\n", (*config_vars)->ifc_elem->type, (char *)(*config_vars)->ifc_elem->name);
                           ret_val = -1;
                           goto end_label;
                        }
                        (*config_vars)->ifc_elem = (*config_vars)->ifc_elem->next;
                     }
                  }

                  (*config_vars)->module_atr_elem = (*config_vars)->module_atr_elem->next;
               }
            }
            (*config_vars)->module_elem = (*config_vars)->module_elem->next;
         }
      } else if ((*config_vars)->current_node->type == XML_COMMENT_NODE || (*config_vars)->current_node->type == XML_TEXT_NODE) {
         // Nothing to do here
      } else {
         /* All other nodes are unexpected and are not allowed */
         VERBOSE(N_STDOUT, "[ERROR] Unexpected node (type: %d, name: %s) in \"nemea-supervisor\" element!\n", (*config_vars)->current_node->type, (char *)(*config_vars)->current_node->name);
         ret_val = -1;
         goto end_label;
      }
      (*config_vars)->current_node = (*config_vars)->current_node->next;
   }

end_label:
   ptr1 = NULL;
   // free linked list of module names
   while (first_module_name != NULL) {
      ptr1 = first_module_name;
      first_module_name = first_module_name->next;
      NULLP_TEST_AND_FREE(ptr1->str)
      NULLP_TEST_AND_FREE(ptr1)
   }
   ptr1 = NULL;
   // free linked list of profile names
   while (first_profile_name != NULL) {
      ptr1 = first_profile_name;
      first_profile_name = first_profile_name->next;
      NULLP_TEST_AND_FREE(ptr1->str)
      NULLP_TEST_AND_FREE(ptr1)
   }
   if (ret_val == 0) {
      VERBOSE(N_STDOUT, "[RELOAD] Validation of the configuration file successfully finished.\n- - -\n");
      return 0;
   }
   VERBOSE(N_STDOUT, "[RELOAD] Validation of the configuration file failed.\n- - -\n");
   return -1;
}


#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>


#define INIT_BUFFER_SIZE 512
#define INC_BUFFER_SIZE 1024

typedef struct buffer_s {
   char *mem;
   unsigned int mem_size;
   unsigned int mem_used;
} buffer_t;

void check_buffer_space(buffer_t *buffer, unsigned int needed_size)
{
   int orig_size = 0;

   if (buffer->mem == NULL) {
      buffer->mem = (char *) calloc(INIT_BUFFER_SIZE + needed_size, sizeof(char));
      buffer->mem_size = INIT_BUFFER_SIZE + needed_size;
      buffer->mem_used = 0;
   } else if ((buffer->mem_size - buffer->mem_used) <= needed_size) {
      orig_size = buffer->mem_size;
      buffer->mem_size += INC_BUFFER_SIZE + needed_size;
      buffer->mem = (char *) realloc(buffer->mem, buffer->mem_size * sizeof(char));
      memset(buffer->mem + orig_size, 0, (buffer->mem_size - orig_size) * sizeof(char));
   }
}

void append_file_content(buffer_t *buffer, char *incl_path)
{
   FILE *fd = fopen(incl_path, "rb");
   if (fd == NULL) {
      return;
   }

   fseek(fd, 0, SEEK_END);
   long fsize = ftell(fd);
   fseek(fd, 0, SEEK_SET);

   check_buffer_space(buffer, fsize);
   if (fread(buffer->mem + buffer->mem_used, fsize, 1, fd) != -1) {
      buffer->mem_used += fsize;
   }
   fclose(fd);
}

void include_item(buffer_t *buffer, char **item_path)
{
   char dir_entry_path[PATH_MAX];
   DIR *dirp;
   struct dirent *dir_entry;

   if (check_file_type_perm(*item_path, CHECK_FILE, R_OK) == 0) {
      append_file_content(buffer, *item_path);
      return;
   } else if (check_file_type_perm(*item_path, CHECK_DIR, R_OK) != 0) {
      // error, item is not a file, nor a dir
      return;
   }

   if ((dirp = opendir(*item_path)) == NULL) {
      return;
   }

   while(1) {
      dir_entry = readdir(dirp);
      if (dir_entry == NULL) {
         break;
      }

      if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0){
         continue;
      }

      if ((*item_path)[strlen((*item_path)) - 1] == '/') {
         if (snprintf(dir_entry_path, sizeof(dir_entry_path), "%s%s", (*item_path), dir_entry->d_name) < 1) {
            continue;
         }
      } else {
         if (snprintf(dir_entry_path, sizeof(dir_entry_path), "%s/%s", (*item_path), dir_entry->d_name) < 1) {
            continue;
         }
      }

      if (check_file_type_perm(dir_entry_path, CHECK_FILE, R_OK) == 0) {
         if (strstr(dir_entry->d_name, ".sup") == NULL) {
            continue;
         } else {
            append_file_content(buffer, dir_entry_path);
         }
      }
   }

   closedir(dirp);
   return;
}


int generate_config_file()
{
   int ret_val = 0;
   char *incl_path = NULL;
   char *line = NULL;
   size_t line_size = 0;
   buffer_t *gener_cont = NULL;
   int pos;

   FILE *templ_fd = fopen(templ_config_file, "r");
   if (templ_fd == NULL) {
      VERBOSE(N_STDOUT, "[ERROR] Could not open \"%s\"\n", templ_config_file);
      return -1;
   }
   FILE *gener_fd = fopen(gener_config_file, "w");
   if (gener_fd == NULL) {
      VERBOSE(N_STDOUT, "[ERROR] Could not open \"%s\"\n", gener_config_file);
      fclose(templ_fd);
      return -1;
   }

   gener_cont = (buffer_t *) calloc(1, sizeof(buffer_t));
   incl_path = (char *) calloc(PATH_MAX, sizeof(char));


   VERBOSE(N_STDOUT, "- - -\n[RELOAD] Generating the configuration file from the template...\n");

   while (1) {
      ret_val = getline(&line, &line_size, templ_fd);
      if (ret_val == -1) {
         break;
      }

      pos = 0;
      while (line[pos] == ' ') {
         pos++;
      }

      if (sscanf(line + pos, "<!-- include %s -->", incl_path) == 1) {
         // append content of every file from dir
         include_item(gener_cont, &incl_path);
      } else {
         // append line
         check_buffer_space(gener_cont, ret_val);
         sprintf(gener_cont->mem + gener_cont->mem_used, "%s", line);
         gener_cont->mem_used += ret_val;
      }
   }

   fprintf(gener_fd, "%s", gener_cont->mem);
   fflush(gener_fd);
   VERBOSE(N_STDOUT, "[RELOAD] The configuration file was successfully generated.\n");

   fclose(gener_fd);
   fclose(templ_fd);
   NULLP_TEST_AND_FREE(gener_cont);
   NULLP_TEST_AND_FREE(incl_path);
   return 0;
}


int reload_configuration(const int choice, xmlNodePtr *node)
{
   pthread_mutex_lock(&running_modules_lock);
   modules_profile_t *ptr1 = NULL, *ptr2 = NULL;
   int ret_val = 0;
   FILE *tmp_err = NULL;
   char *backup_file_name = NULL;
   int modules_got_profile;
   unsigned int x = 0;
   int number = 0;
   unsigned int original_loaded_modules_cnt = loaded_modules_cnt;
   int ifc_cnt = 0;
   reload_config_vars_t * config_vars = (reload_config_vars_t *) calloc(1, sizeof(reload_config_vars_t));
   xmlChar *key = NULL;

   switch (choice) {
      case RELOAD_INIT_LOAD_CONFIG: {
            backup_file_name = create_backup_file_path();
            if (backup_file_name == NULL) {
parse_default_config_file:
               if (generate_config_file() == -1) {
                  VERBOSE(N_STDOUT, "%s [ERROR] Could not generate configuration file with path \"%s\"!\n", get_formatted_time(), gener_config_file);
                  NULLP_TEST_AND_FREE(backup_file_name)
                  pthread_mutex_unlock(&running_modules_lock);
                  xmlCleanupParser();
                  free(config_vars);
                  return FALSE;
               }
               tmp_err = stderr;
               stderr = supervisor_debug_log_fd;
               config_vars->doc_tree_ptr = xmlParseFile(gener_config_file);
               stderr = tmp_err;
               if (config_vars->doc_tree_ptr == NULL) {
                  VERBOSE(N_STDOUT, "%s [ERROR] Could not parse generated configuration file with path \"%s\"!\n", get_formatted_time(), gener_config_file);
                  NULLP_TEST_AND_FREE(backup_file_name)
                  pthread_mutex_unlock(&running_modules_lock);
                  xmlCleanupParser();
                  free(config_vars);
                  return FALSE;
               }
            } else {
               tmp_err = stderr;
               stderr = supervisor_debug_log_fd;
               config_vars->doc_tree_ptr = xmlParseFile(backup_file_name);
               stderr = tmp_err;
               if (config_vars->doc_tree_ptr == NULL) {
                  if (access(backup_file_name, R_OK) == -1) {
                     if (errno == EACCES) {
                        VERBOSE(N_STDOUT, "%s [WARNING] I don't have permissions to read backup file with path \"%s\", I'm gonna generate a new configuration!\n", get_formatted_time(), backup_file_name);
                     }
                  } else {
                     VERBOSE(N_STDOUT,"%s [WARNING] Backup file with path \"%s\" was not parsed successfully, I'm gonna generate a new configuration!\n", get_formatted_time(), backup_file_name);
                  }
                  goto parse_default_config_file;
               } else {
                  VERBOSE(N_STDOUT, "%s [INFO] Loading backup file for this configuration template...\n", get_formatted_time());
                  // delete backup file after parsing, it wont be needed anymore
                  if (unlink(backup_file_name) == -1) {
                     if (errno == EACCES) {
                        VERBOSE(N_STDOUT, "%s [WARNING] I don't have permissions to delete backup file \"%s\"\n", get_formatted_time(), backup_file_name);
                     }
                  }
               }
            }
            NULLP_TEST_AND_FREE(backup_file_name)
            config_vars->root_node = xmlDocGetRootElement(config_vars->doc_tree_ptr);
         }
         break;

      case RELOAD_DEFAULT_CONFIG_FILE:
         if (generate_config_file() == -1) {
            VERBOSE(N_STDOUT, "%s [ERROR] Could not generate configuration file with path \"%s\"!\n", get_formatted_time(), gener_config_file);
            NULLP_TEST_AND_FREE(backup_file_name)
            pthread_mutex_unlock(&running_modules_lock);
            xmlCleanupParser();
            free(config_vars);
            return FALSE;
         }
         tmp_err = stderr;
         stderr = supervisor_debug_log_fd;
         config_vars->doc_tree_ptr = xmlParseFile(gener_config_file);
         stderr = tmp_err;
         if (config_vars->doc_tree_ptr == NULL) {
            VERBOSE(N_STDOUT, "%s [ERROR] Could not parse generated configuration file with path \"%s\"!\n", get_formatted_time(), gener_config_file);
            pthread_mutex_unlock(&running_modules_lock);
            xmlCleanupParser();
            free(config_vars);
            return FALSE;
         }
         config_vars->root_node = xmlDocGetRootElement(config_vars->doc_tree_ptr);
         break;

      case RELOAD_CALLBACK_ROOT_ELEM:
         config_vars->root_node = *node;
         break;

      default:
         xmlCleanupParser();
         pthread_mutex_unlock(&running_modules_lock);
         free(config_vars);
         return FALSE;
   }

   // Validate configuration file
   ret_val = validate_configuration(&config_vars);

   if (ret_val == -1) { // error
      // do not free libnetconf xml structures or parsers data!!!
      if (choice != RELOAD_CALLBACK_ROOT_ELEM) {
         xmlFreeDoc(config_vars->doc_tree_ptr);
         xmlCleanupParser();
      }
      pthread_mutex_unlock(&running_modules_lock);
      free(config_vars);
      return FALSE;
   }



   // Print XML configuration to supervisor debug log
   VERBOSE(DEBUG, "\n\n%s [DEBUG] Request to reload this configuration --->\n\n", get_formatted_time());
   print_xmlDoc_to_stream(config_vars->root_node->doc, supervisor_debug_log_fd);

   config_vars->current_node = config_vars->root_node->xmlChildrenNode;

   /*****************/
   for (x=0; x<running_modules_array_size; x++) {
      running_modules[x].module_checked_by_reload = FALSE;
      running_modules[x].module_modified_by_reload = FALSE;
      running_modules[x].modules_profile = NULL;
      running_modules[x].module_max_restarts_per_minute = -1;
      running_modules[x].module_is_my_child = TRUE;
      running_modules[x].init_module = FALSE;
      running_modules[x].remove_module = FALSE;
   }

   loaded_profile_cnt = 0;
   ptr1 = first_profile_ptr;
   ptr2 = NULL;
   while (ptr1 != NULL) {
      ptr2 = ptr1;
      ptr1 = ptr1->next;
      NULLP_TEST_AND_FREE(ptr2->profile_name)
      NULLP_TEST_AND_FREE(ptr2)
   }
   first_profile_ptr = NULL;
   actual_profile_ptr = NULL;

   /*****************/
   VERBOSE(N_STDOUT,"[RELOAD] Processing new configuration...\n");

   while (config_vars->current_node != NULL) {
      if (!xmlStrcmp(config_vars->current_node->name, BAD_CAST "supervisor")) {
         // Process root's element "supervisor"
         config_vars->module_elem = config_vars->current_node->xmlChildrenNode;
         reload_process_supervisor_element(&config_vars);
      } else if (!xmlStrcmp(config_vars->current_node->name, BAD_CAST "modules")) {
         // Process root's element "modules"
         modules_got_profile = FALSE;
         config_vars->module_elem = config_vars->current_node->xmlChildrenNode;
         config_vars->module_atr_elem = NULL, config_vars->ifc_elem = NULL, config_vars->ifc_atr_elem = NULL;
         ifc_cnt = 0;

         /* if return value equals -1, modules element doesn't have one valid name and enabled element -> it's children (module elements) won't have profile
         *  return value 0 means success -> modules children will have a profile
         */
         if (reload_find_and_check_modules_profile_basic_elements(&config_vars) == 0 && actual_profile_ptr != NULL) {
            VERBOSE(N_STDOUT, "[INFO] Found valid modules profile with name \"%s\" set to %s.\n", actual_profile_ptr->profile_name, (actual_profile_ptr->profile_enabled == TRUE ? "enabled" : "disabled"));
            modules_got_profile = TRUE;
            loaded_profile_cnt++;
         } else {
            modules_got_profile = FALSE;
         }

         config_vars->module_elem = config_vars->current_node->xmlChildrenNode;

         while (config_vars->module_elem != NULL) {
            if (!xmlStrcmp(config_vars->module_elem->name, BAD_CAST "module")) {
               // Process modules element "module"
               config_vars->current_module_idx = -1;

               // Check and reallocate (if needed) running_modules memory
               check_running_modules_allocated_memory();

               config_vars->module_atr_elem = config_vars->module_elem->xmlChildrenNode;

               /* if return value equals 1, there is no more valid module elements -> break the module parsing loop
               *  return value 0 is success -> parse the module attributes
               */
               if (reload_find_and_check_module_basic_elements(&config_vars) == -1 || config_vars->current_module_idx < 0) {
                  // VERBOSE(N_STDOUT, "[WARNING] Reloading error - last module is invalid, breaking the loop.\n");
                  break;
               } else {
                  running_modules[config_vars->current_module_idx].module_checked_by_reload = TRUE;
               }

               // Get module's PID from "module" element if it exists
               if (choice == RELOAD_INIT_LOAD_CONFIG) {
                  key = xmlGetProp(config_vars->module_elem, BAD_CAST "module_pid");
                  if (key != NULL) {
                     running_modules[config_vars->current_module_idx].module_pid = atoi((char *) key);
                     xmlFree(key);
                     key = NULL;
                  }
               }

               config_vars->module_atr_elem = config_vars->module_elem->xmlChildrenNode;
               while (config_vars->module_atr_elem != NULL) {
                  if ((!xmlStrcmp(config_vars->module_atr_elem->name,BAD_CAST "enabled"))) {
                     // Process module's "enabled" attribute
                     reload_resolve_module_enabled(&config_vars, modules_got_profile);
                  } else if (!xmlStrcmp(config_vars->module_atr_elem->name, BAD_CAST "module-restarts")) {
                     // Process module's "module-restarts" attribute
                     key = xmlNodeListGetString(config_vars->doc_tree_ptr, config_vars->module_atr_elem->xmlChildrenNode, 1);
                     if (key != NULL) {
                        x = 0;
                        if ((sscanf((char *) key,"%d",&number) == 1) && (number >= 0)) {
                           x = (unsigned int) number;
                           running_modules[config_vars->current_module_idx].module_max_restarts_per_minute = x;
                        }
                        xmlFree(key);
                        key = NULL;
                     }
                  } else if ((!xmlStrcmp(config_vars->module_atr_elem->name,BAD_CAST "params"))) {
                     // Process module's "parameters" attribute
                     reload_process_module_atribute(&config_vars, &running_modules[config_vars->current_module_idx].module_params);
                  } else if ((!xmlStrcmp(config_vars->module_atr_elem->name,BAD_CAST "name"))) {
                     // Process module's "name" attribute
                     if (config_vars->new_module == TRUE) {
                        key = xmlNodeListGetString(config_vars->doc_tree_ptr, config_vars->module_atr_elem->xmlChildrenNode, 1);
                        if (key != NULL) {
                           running_modules[config_vars->current_module_idx].module_name = (char *) xmlStrdup(key);
                           xmlFree(key);
                           key = NULL;
                        }
                     }
                  } else if ((!xmlStrcmp(config_vars->module_atr_elem->name,BAD_CAST "path"))) {
                     // Process module's "path" attribute
                     reload_process_module_atribute(&config_vars, &running_modules[config_vars->current_module_idx].module_path);
                  } else if ((!xmlStrcmp(config_vars->module_atr_elem->name,BAD_CAST "trapinterfaces"))) {
                     // Process module's "trapinterfaces" element
                     ifc_cnt=0;
                     config_vars->ifc_elem = config_vars->module_atr_elem->xmlChildrenNode;

                     // If the parsed module has been already in configuration, check it's interfaces count -> if original count equals actual, it's ok, otherwise interfaces will be updated.
                     if (config_vars->new_module == FALSE) {
                        reload_check_modules_interfaces_count(&config_vars);
                     }

                     while (config_vars->ifc_elem != NULL) {
                        if (!xmlStrcmp(config_vars->ifc_elem->name,BAD_CAST "interface")) {
                           config_vars->ifc_atr_elem = config_vars->ifc_elem->xmlChildrenNode;

                           // Check and reallocate (if needed) module's interfaces array
                           reload_check_module_allocated_interfaces(config_vars->current_module_idx, ifc_cnt);

                           while (config_vars->ifc_atr_elem != NULL) {
                              if ((!xmlStrcmp(config_vars->ifc_atr_elem->name,BAD_CAST "note"))) {
                                 // Process module's interface "note" attribute
                                 if (reload_process_module_interface_atribute(&config_vars, &running_modules[config_vars->current_module_idx].module_ifces[ifc_cnt].ifc_note) == -1) {
                                    ifc_cnt = -1;
                                    break;
                                 }
                              } else if ((!xmlStrcmp(config_vars->ifc_atr_elem->name,BAD_CAST "type"))) {
                                 // Process module's interface "type" attribute
                                 if (reload_process_module_interface_atribute(&config_vars, &running_modules[config_vars->current_module_idx].module_ifces[ifc_cnt].ifc_type) == -1) {
                                    ifc_cnt = -1;
                                    break;
                                 }
                              } else if ((!xmlStrcmp(config_vars->ifc_atr_elem->name,BAD_CAST "direction"))) {
                                 // Process module's interface "direction" attribute
                                 if (reload_process_module_interface_atribute(&config_vars, &running_modules[config_vars->current_module_idx].module_ifces[ifc_cnt].ifc_direction) == -1) {
                                    ifc_cnt = -1;
                                    break;
                                 }
                              } else if ((!xmlStrcmp(config_vars->ifc_atr_elem->name,BAD_CAST "params"))) {
                                 // Process module's interface "parameters" attribute
                                 if (reload_process_module_interface_atribute(&config_vars, &running_modules[config_vars->current_module_idx].module_ifces[ifc_cnt].ifc_params) == -1) {
                                    ifc_cnt = -1;
                                    break;
                                 }
                              }
                              config_vars->ifc_atr_elem=config_vars->ifc_atr_elem->next;
                           }

                           ifc_cnt++;
                           if (config_vars->module_ifc_insert == TRUE) {
                              running_modules[config_vars->current_module_idx].module_ifces_cnt++;
                           }
                        }
                        config_vars->ifc_elem = config_vars->ifc_elem->next;
                     }
                  }

                  config_vars->module_atr_elem = config_vars->module_atr_elem->next;
               }

               // If the parsed module is new or it's interfaces were updated, count it's input and output interfaces
               if (config_vars->module_ifc_insert == TRUE) {
                  reload_count_module_interfaces(&config_vars);
               }

               // If the parsed module is new (it was inserted to next empty position), increment counters
               if (config_vars->current_module_idx == loaded_modules_cnt) {
                  loaded_modules_cnt++;
                  config_vars->inserted_modules++;
               }

               // If actual modules element has a valid profile, save it's name in parsed module structure
               if (modules_got_profile == TRUE) {
                  running_modules[config_vars->current_module_idx].modules_profile = actual_profile_ptr;
               }
            }
            config_vars->module_elem = config_vars->module_elem->next;
         }
      }
      config_vars->current_node = config_vars->current_node->next;
   }

   // do not free libnetconf xml structures or parsers data!!!
   if (choice != RELOAD_CALLBACK_ROOT_ELEM) {
      xmlFreeDoc(config_vars->doc_tree_ptr);
      xmlCleanupParser();
   }

   // Stop and remove missing modules from loaded configuration (modules deleted by user)
   for (x = 0; x < original_loaded_modules_cnt; x++) {
      if (running_modules[x].module_checked_by_reload == FALSE) {
         VERBOSE(N_STDOUT, "[WARNING] %s was not found in new configuration, it will be stopped and removed.\n", running_modules[x].module_name);
         running_modules[x].module_enabled = FALSE;
         running_modules[x].remove_module = TRUE;
         config_vars->removed_modules++;
      }
   }

   for (x=0; x<loaded_modules_cnt; x++) {
      running_modules[x].module_served_by_service_thread = FALSE;
      // Count modified modules
      if (running_modules[x].module_modified_by_reload == TRUE) {
         config_vars->modified_modules++;
         // If they were or are running, restart them with new configuration and initialize their variables
         if (running_modules[x].module_running == TRUE) {
            running_modules[x].module_running = FALSE;
            if (running_modules[x].module_enabled == TRUE) {
               VERBOSE(N_STDOUT, "[WARNING] %s was modified by reload and it has been running -> it will be restarted with new configuration.\n", running_modules[x].module_name);
               running_modules[x].module_enabled = FALSE;
               running_modules[x].init_module = TRUE;
            }
         }
      }
   }

   // If module's PID > 0, initialize it's variables, because it's already running, so there won't be re_start function call
   if (choice == RELOAD_INIT_LOAD_CONFIG) {
      for (x=0; x<loaded_modules_cnt; x++) {
         if (running_modules[x].module_pid > 0) {
            init_module_variables(x);
         }
      }
   }

   // Print reload statistics
   VERBOSE(N_STDOUT, "\n[RELOAD] Result:\n");
   VERBOSE(N_STDOUT, "Inserted modules:\t%d\n", config_vars->inserted_modules);
   VERBOSE(N_STDOUT, "Removed modules:\t%d\n", config_vars->removed_modules);
   VERBOSE(N_STDOUT, "Modified modules:\t%d\n", config_vars->modified_modules);
   VERBOSE(N_STDOUT, "Unmodified modules:\t%d\n", original_loaded_modules_cnt - config_vars->modified_modules - config_vars->removed_modules);
   VERBOSE(N_STDOUT, "[RELOAD] Processing of the new configuration successfully finished.\n- - -\n");
   pthread_mutex_unlock(&running_modules_lock);
   free(config_vars);
   return TRUE;
}


/*****************************************************************
 * Netconf functions *
 *****************************************************************/

#ifdef nemea_plugin
void *netconf_server_routine_thread(void *arg)
{
   daemon_mode_server_routine();
   pthread_exit(EXIT_SUCCESS);
}

// Nemea plugin initialization method
int netconf_supervisor_initialization(xmlNodePtr *running)
{
   pthread_attr_t attr;
   pthread_attr_init(&attr);

   init_sup_flags();
   netconf_flag = TRUE;
   socket_path = DEFAULT_NETCONF_SERVER_SOCKET;

   if (daemon_init_structures() != 0) {
      return -1;
   }
   if (daemon_init_socket() != 0) {
      return -1;
   }
   daemon_mode_initialized = TRUE;

   if (supervisor_initialization() != 0) {
      return -1;
   }

   // Load startup configuration
   reload_configuration(RELOAD_CALLBACK_ROOT_ELEM, running);

   // Create thread doing server routine
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&netconf_server_thread_id,  &attr, netconf_server_routine_thread, NULL) != 0) {
      return -1;
   }
   return 0;
}

xmlDocPtr netconf_get_state_data()
{
   modules_profile_t * ptr = first_profile_ptr;
   unsigned int x = 0, y = 0, modules_with_profile = 0;
   char buffer[DEFAULT_SIZE_OF_BUFFER];
   const char *template = "<?xml version=\"1.0\"?><nemea-supervisor xmlns=\"urn:cesnet:tmc:nemea:1.0\"></nemea-supervisor>";
   xmlDocPtr doc_tree_ptr = NULL;
   xmlNodePtr root_elem = NULL, modules_elem = NULL, module_elem = NULL, trapinterfaces_elem = NULL, interface_elem = NULL;
   xmlNodePtr avail_modules = NULL, binpaths = NULL, param = NULL;

   if (loaded_modules_cnt > 0 || first_available_modules_path != NULL) {
      doc_tree_ptr = xmlParseMemory(template, strlen(template));
      if (doc_tree_ptr == NULL) {
         return NULL;
      }
      root_elem = xmlDocGetRootElement(doc_tree_ptr);
      if (root_elem == NULL) {
         return NULL;
      }

      if (first_available_modules_path != NULL) {
      avail_modules = xmlNewChild(root_elem, NULL, "available-modules", NULL);
      modules_elem = xmlNewChild(avail_modules, NULL, "modules", NULL);

      available_modules_path_t * avail_path = first_available_modules_path;
      available_module_t * avail_path_modules = NULL;

      while (avail_path != NULL) {
         avail_path_modules = avail_path->modules;
         while (avail_path_modules != NULL) {
            module_elem = xmlNewChild(modules_elem, NULL, "module", NULL);
            xmlNewChild(module_elem, NULL, "name", BAD_CAST avail_path_modules->name);
            if (avail_path_modules->module_info != NULL) {
               xmlNewChild(module_elem, NULL, "description", BAD_CAST avail_path_modules->module_info->description);
               memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER);
               snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%d", avail_path_modules->module_info->num_ifc_in);
               xmlNewChild(module_elem, NULL, "number-in-ifc", BAD_CAST buffer);
               memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER);
               snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%d", avail_path_modules->module_info->num_ifc_out);
               xmlNewChild(module_elem, NULL, "number-out-ifc", BAD_CAST buffer);

               // Process module parameters
               x=0;
               while (avail_path_modules->module_info->params[x] != NULL) {
                  param = xmlNewChild(module_elem, NULL, "parameter", NULL);
                  memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER);
                  sprintf(buffer, "-%c", avail_path_modules->module_info->params[x]->short_opt);
                  xmlNewChild(param, NULL, "short-opt", BAD_CAST buffer);
                  memset(buffer, 0, DEFAULT_SIZE_OF_BUFFER);
                  sprintf(buffer, "--%s", avail_path_modules->module_info->params[x]->long_opt);
                  xmlNewChild(param, NULL, "long-opt", BAD_CAST buffer);
                  xmlNewChild(param, NULL, "description", BAD_CAST avail_path_modules->module_info->params[x]->description);
                  if (avail_path_modules->module_info->params[x]->param_required_argument == TRUE) {
                     xmlNewChild(param, NULL, "mandatory-argument", BAD_CAST "true");
                  } else {
                     xmlNewChild(param, NULL, "mandatory-argument", BAD_CAST "false");
                  }
                  xmlNewChild(param, NULL, "argument-type", BAD_CAST avail_path_modules->module_info->params[x]->argument_type);
                  x++;
               }
            }
            avail_path_modules = avail_path_modules->next;
         }
         avail_path = avail_path->next;
      }
      }

   if (loaded_modules_cnt > 0) {
      // get state data about modules with a profile
      while (ptr != NULL) {
         if (ptr->profile_name != NULL) {
            modules_elem = xmlNewChild(root_elem, NULL, BAD_CAST "modules", NULL);
            xmlNewChild(modules_elem, NULL, BAD_CAST "name", BAD_CAST ptr->profile_name);
            for (x = 0; x < loaded_modules_cnt; x++) {
               if (running_modules[x].modules_profile == NULL) {
                  continue;
               }
               if (strcmp(running_modules[x].modules_profile->profile_name, ptr->profile_name) != 0) {
                  continue;
               }
               memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
               module_elem = xmlNewChild(modules_elem, NULL, BAD_CAST "module", NULL);
               xmlNewChild(module_elem, NULL, BAD_CAST "name", BAD_CAST running_modules[x].module_name);

               if (running_modules[x].module_status == TRUE) {
                  xmlNewChild(module_elem, NULL, BAD_CAST "running", BAD_CAST "true");
               } else {
                  xmlNewChild(module_elem, NULL, BAD_CAST "running", BAD_CAST "false");
               }

               if (running_modules[x].module_restart_cnt < 0) {
                  snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%d",0);
                  xmlNewChild(module_elem, NULL, BAD_CAST "restart-counter", BAD_CAST buffer);
               } else {
                  snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%d",running_modules[x].module_restart_cnt);
                  xmlNewChild(module_elem, NULL, BAD_CAST "restart-counter", BAD_CAST buffer);
               }

               if (running_modules[x].module_service_ifc_isconnected == TRUE && running_modules[x].module_status) {
                  trapinterfaces_elem = xmlNewChild(module_elem, NULL, BAD_CAST "trapinterfaces", NULL);
                  for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
                     if (running_modules[x].module_ifces[y].int_ifc_direction != INVALID_MODULE_IFC_ATTR && running_modules[x].module_ifces[y].ifc_params != NULL && running_modules[x].module_ifces[y].int_ifc_type != INVALID_MODULE_IFC_ATTR) {
                        interface_elem = xmlNewChild(trapinterfaces_elem, NULL, BAD_CAST "interface", NULL);
                        xmlNewChild(interface_elem, NULL, BAD_CAST "type", BAD_CAST running_modules[x].module_ifces[y].ifc_type);
                        xmlNewChild(interface_elem, NULL, BAD_CAST "direction", BAD_CAST running_modules[x].module_ifces[y].ifc_direction);
                        xmlNewChild(interface_elem, NULL, BAD_CAST "params", BAD_CAST running_modules[x].module_ifces[y].ifc_params);
                        if (running_modules[x].module_ifces[y].int_ifc_direction == IN_MODULE_IFC_DIRECTION) {
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((in_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->recv_buffer_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-buffer-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((in_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->recv_msg_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-msg-cnt", BAD_CAST buffer);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-msg-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "dropped-msg-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-buffer-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "autoflush-cnt", BAD_CAST "0");
                        } else if (running_modules[x].module_ifces[y].int_ifc_direction == OUT_MODULE_IFC_DIRECTION) {
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-buffer-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-msg-cnt", BAD_CAST "0");
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->sent_msg_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-msg-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->dropped_msg_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "dropped-msg-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->sent_buffer_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-buffer-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->autoflush_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "autoflush-cnt", BAD_CAST buffer);
                        }
                     }
                  }
               }

               /* TODO check and free */
               if (xmlAddChild(modules_elem, module_elem) == NULL) {
                  xmlFree(module_elem);
               }
               modules_with_profile++;
            }

            if (xmlAddChild(root_elem, modules_elem) == NULL) {
               xmlFree(modules_elem);
            }
         }
         ptr = ptr->next;
      }


      //get state data about modules without profile
      if (modules_with_profile < loaded_modules_cnt) {
         modules_elem = xmlNewChild(root_elem, NULL, BAD_CAST "modules", NULL);
         for (x = 0; x < loaded_modules_cnt; x++) {
               if (running_modules[x].modules_profile != NULL) {
                  continue;
               }
               memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
               module_elem = xmlNewChild(modules_elem, NULL, BAD_CAST "module", NULL);
               xmlNewChild(module_elem, NULL, BAD_CAST "name", BAD_CAST running_modules[x].module_name);

               if (running_modules[x].module_status == TRUE) {
                  xmlNewChild(module_elem, NULL, BAD_CAST "running", BAD_CAST "true");
               } else {
                  xmlNewChild(module_elem, NULL, BAD_CAST "running", BAD_CAST "false");
               }

               if (running_modules[x].module_restart_cnt < 0) {
                  snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%d",0);
                  xmlNewChild(module_elem, NULL, BAD_CAST "restart-counter", BAD_CAST buffer);
               } else {
                  snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%d",running_modules[x].module_restart_cnt);
                  xmlNewChild(module_elem, NULL, BAD_CAST "restart-counter", BAD_CAST buffer);
               }

               if (running_modules[x].module_service_ifc_isconnected == TRUE && running_modules[x].module_status) {
                  trapinterfaces_elem = xmlNewChild(module_elem, NULL, BAD_CAST "trapinterfaces", NULL);
                  for (y=0; y<running_modules[x].module_ifces_cnt; y++) {
                     if (running_modules[x].module_ifces[y].int_ifc_direction != INVALID_MODULE_IFC_ATTR && running_modules[x].module_ifces[y].ifc_params != NULL && running_modules[x].module_ifces[y].int_ifc_type != INVALID_MODULE_IFC_ATTR) {
                        interface_elem = xmlNewChild(trapinterfaces_elem, NULL, BAD_CAST "interface", NULL);
                        xmlNewChild(interface_elem, NULL, BAD_CAST "type", BAD_CAST running_modules[x].module_ifces[y].ifc_type);
                        xmlNewChild(interface_elem, NULL, BAD_CAST "direction", BAD_CAST running_modules[x].module_ifces[y].ifc_direction);
                        xmlNewChild(interface_elem, NULL, BAD_CAST "params", BAD_CAST running_modules[x].module_ifces[y].ifc_params);
                        if (running_modules[x].module_ifces[y].int_ifc_direction == IN_MODULE_IFC_DIRECTION) {
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((in_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->recv_buffer_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-buffer-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((in_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->recv_msg_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-msg-cnt", BAD_CAST buffer);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-msg-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "dropped-msg-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-buffer-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "autoflush-cnt", BAD_CAST "0");
                        } else if (running_modules[x].module_ifces[y].int_ifc_direction == OUT_MODULE_IFC_DIRECTION) {
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-buffer-cnt", BAD_CAST "0");
                           xmlNewChild(interface_elem, NULL, BAD_CAST "recv-msg-cnt", BAD_CAST "0");
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->sent_msg_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-msg-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->dropped_msg_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "dropped-msg-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->sent_buffer_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "sent-buffer-cnt", BAD_CAST buffer);
                           memset(buffer,0,DEFAULT_SIZE_OF_BUFFER);
                           snprintf(buffer, DEFAULT_SIZE_OF_BUFFER, "%"PRIu64, ((out_ifc_stats_t *) running_modules[x].module_ifces[y].ifc_data)->autoflush_cnt);
                           xmlNewChild(interface_elem, NULL, BAD_CAST "autoflush-cnt", BAD_CAST buffer);
                        }
                     }
                  }
               }

               /* TODO check and free */
               if (xmlAddChild(modules_elem, module_elem) == NULL) {
                  xmlFree(module_elem);
               }
            }

            if (xmlAddChild(root_elem, modules_elem) == NULL) {
               xmlFree(modules_elem);
            }
         }
      }
   }
   xmlCleanupParser();
   return doc_tree_ptr;
}
#endif
