#include <jni.h>
#include "org_uiautomation_iosdriver_services_DeviceInstallerService.h"


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#define _GNU_SOURCE 1
#define __USE_GNU 1
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <libgen.h>
#include <inttypes.h>
#include <limits.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/notification_proxy.h>
#include <libimobiledevice/house_arrest.h>
#include <libimobiledevice/afc.h>

#include <plist/plist.h>

#include <zip.h>

#include <sys/stat.h>

#if !defined(S_IFLNK)
#define S_IFLNK 0120000
#endif
#if !defined(S_IFSOCK)
#define S_IFSOCK 0140000
#endif

const char PKG_PATH[] = "PublicStaging";
const char APPARCH_PATH[] = "ApplicationArchives";

char *uuid = NULL;
char *options = NULL;
char *appid = NULL;

int list_apps_mode = 0;
int install_mode = 0;
int uninstall_mode = 0;
int upgrade_mode = 0;
int list_archives_mode = 0;
int archive_mode = 0;
int restore_mode = 0;
int remove_archive_mode = 0;

char *last_status = NULL;
int wait_for_op_complete = 0;
int notification_expected = 0;
int op_completed = 0;
int err_occured = 0;
int notified = 0;

static JavaVM *jvm;


static void notifier(const char *notification, void *unused){
	notified = 1;
}

static void status_cb(const char *operation, plist_t status,void * idvoid){

    char const * id = (char const *) idvoid;
    JNIEnv *env;
    if (jvm ==NULL){
        printf("Initialize first..\n");
    }
    (*jvm)->AttachCurrentThread(jvm, (void **)&env, NULL);

    if (env==NULL){
        printf("Need to specific the JNI env for logging.\n");
    }

	if (status && operation) {
		plist_t npercent = plist_dict_get_item(status, "PercentComplete");
		plist_t nstatus = plist_dict_get_item(status, "Status");
		plist_t nerror = plist_dict_get_item(status, "Error");
		int percent = 0;
		char *status_msg = NULL;
		if (npercent) {
			uint64_t val = 0;
			plist_get_uint_val(npercent, &val);
			percent = val;
		}
		if (nstatus) {
			plist_get_string_val(nstatus, &status_msg);
			if (!strcmp(status_msg, "Complete")) {
				op_completed = 1;
			}
		}
		if (!nerror) {
			if (last_status && (strcmp(last_status, status_msg))) {
				printf("\n");
			}

			if (!npercent) {
				logInfo("%s;%s;%s", id, operation, status_msg);
            } else {
				logInfo( "%s;%s;%s;%d%%\r", id, operation, status_msg, percent);
			}
		} else {
			char *err_msg = NULL;
			plist_get_string_val(nerror, &err_msg);
			logInfo("%s;%s;Error occured: %s", id, operation, err_msg);
			free(err_msg);
			err_occured = 1;
		}
		if (last_status) {
			free(last_status);
			last_status = NULL;
		}
		if (status_msg) {
			last_status = strdup(status_msg);
			free(status_msg);
		}
	} else {
		printf("%s: called with invalid data!\n", __func__);
	}
}

static int zip_f_get_contents(struct zip *zf, const char *filename, int locate_flags, char **buffer, uint32_t *len)
{
	struct zip_stat zs;
	struct zip_file *zfile;
	int zindex = zip_name_locate(zf, filename, locate_flags);

	*buffer = NULL;
	*len = 0;

	if (zindex < 0) {
		fprintf(stderr, "ERROR: could not locate %s in archive!\n", filename);
		return -1;
	}

	zip_stat_init(&zs);

	if (zip_stat_index(zf, zindex, 0, &zs) != 0) {
		fprintf(stderr, "ERROR: zip_stat_index '%s' failed!\n", filename);
		return -2;
	}

	if (zs.size > 10485760) {
		fprintf(stderr, "ERROR: file '%s' is too large!\n", filename);
		return -3;
	}

	zfile = zip_fopen_index(zf, zindex, 0);
	if (!zfile) {
		fprintf(stderr, "ERROR: zip_fopen '%s' failed!\n", filename);
		return -4;
	}

	*buffer = malloc(zs.size);
	if (zs.size > LLONG_MAX || zip_fread(zfile, *buffer, zs.size) != (zip_int64_t)zs.size) {
		fprintf(stderr, "ERROR: zip_fread %" PRIu64 " bytes from '%s'\n", (uint64_t)zs.size, filename);
		free(*buffer);
		*buffer = NULL;
		zip_fclose(zfile);
		return -5;
	}
	*len = zs.size;
	zip_fclose(zfile);
	return 0;
}

static void do_wait_when_needed()
{
	int i = 0;
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 500000000;

	/* wait for operation to complete */
	while (wait_for_op_complete && !op_completed && !err_occured
		   && !notified && (i < 60)) {
		nanosleep(&ts, NULL);
		i++;
	}

	/* wait some time if a notification is expected */
	while (notification_expected && !notified && !err_occured && (i < 10)) {
		nanosleep(&ts, NULL);
		i++;
	}
}

static void print_usage(int argc, char **argv)
{
	char *name = NULL;

	name = strrchr(argv[0], '/');
	printf("Usage: %s OPTIONS\n", (name ? name + 1 : argv[0]));
	printf("Manage apps on an iDevice.\n\n");
	printf
		("  -U, --uuid UUID\tTarget specific device by its 40-digit device UUID.\n"
		 "  -l, --list-apps\tList apps, possible options:\n"
		 "       -o list_user\t- list user apps only (this is the default)\n"
		 "       -o list_system\t- list system apps only\n"
		 "       -o list_all\t- list all types of apps\n"
		 "       -o xml\t\t- print full output as xml plist\n"
		 "  -i, --install ARCHIVE\tInstall app from package file specified by ARCHIVE.\n"
		 "  -u, --uninstall APPID\tUninstall app specified by APPID.\n"
		 "  -g, --upgrade APPID\tUpgrade app specified by APPID.\n"
		 "  -L, --list-archives\tList archived applications, possible options:\n"
		 "       -o xml\t\t- print full output as xml plist\n"
		 "  -a, --archive APPID\tArchive app specified by APPID, possible options:\n"
		 "       -o uninstall\t- uninstall the package after making an archive\n"
		 "       -o app_only\t- archive application data only\n"
		 "       -o copy=PATH\t- copy the app archive to directory PATH when done\n"
		 "       -o remove\t- only valid when copy=PATH is used: remove after copy\n"
		 "  -r, --restore APPID\tRestore archived app specified by APPID\n"
		 "  -R, --remove-archive APPID  Remove app archive specified by APPID\n"
		 "  -o, --options\t\tPass additional options to the specified command.\n"
		 "  -h, --help\t\tprints usage information\n"
		 "  -d, --debug\t\tenable communication debugging\n" "\n");
}

static void parse_opts(int argc, char **argv)
{
	static struct option longopts[] = {
		{"help", 0, NULL, 'h'},
		{"uuid", 1, NULL, 'U'},
		{"list-apps", 0, NULL, 'l'},
		{"install", 1, NULL, 'i'},
		{"uninstall", 1, NULL, 'u'},
		{"upgrade", 1, NULL, 'g'},
		{"list-archives", 0, NULL, 'L'},
		{"archive", 1, NULL, 'a'},
		{"restore", 1, NULL, 'r'},
		{"remove-archive", 1, NULL, 'R'},
		{"options", 1, NULL, 'o'},
		{"debug", 0, NULL, 'd'},
		{NULL, 0, NULL, 0}
	};
	// freynaud : this function will be called multiple times. Needs to reset the index for parsing.
	optind=1;
	list_apps_mode = 0;
    install_mode = 0;
    uninstall_mode = 0;
    upgrade_mode = 0;
    list_archives_mode = 0;
    archive_mode = 0;
    restore_mode = 0;
    remove_archive_mode = 0;

    last_status = NULL;
    wait_for_op_complete = 0;
    notification_expected = 0;
    op_completed = 0;
    err_occured = 0;
    notified = 0;



	int c;

	while (1) {
		c = getopt_long(argc, argv, "hU:li:u:g:La:r:R:o:d", longopts,
						(int *) 0);
		if (c == -1) {
			break;
		}
        switch (c) {
		case 'h':
			print_usage(argc, argv);
			exit(0);
		case 'U':
			if (strlen(optarg) != 40) {
				printf("%s: invalid UUID specified (length != 40)\n",
					   argv[0]);
				print_usage(argc, argv);
				exit(2);
			}
			uuid = strdup(optarg);
			break;
		case 'l':
			list_apps_mode = 1;
			break;
		case 'i':
			install_mode = 1;
			appid = strdup(optarg);
			break;
		case 'u':
			uninstall_mode = 1;
			appid = strdup(optarg);
			break;
		case 'g':
			upgrade_mode = 1;
			appid = strdup(optarg);
			break;
		case 'L':
			list_archives_mode = 1;
			break;
		case 'a':
			archive_mode = 1;
			appid = strdup(optarg);
			break;
		case 'r':
			restore_mode = 1;
			appid = strdup(optarg);
			break;
		case 'R':
			remove_archive_mode = 1;
			appid = strdup(optarg);
			break;
		case 'o':
			if (!options) {
				options = strdup(optarg);
			} else {
				char *newopts =	malloc(strlen(options) + strlen(optarg) + 2);
				strcpy(newopts, options);
				free(options);
				strcat(newopts, ",");
				strcat(newopts, optarg);
				options = newopts;
			}
			break;
		case 'd':
			idevice_set_debug_level(1);
			break;
		default:
			print_usage(argc, argv);
			exit(2);
		}
	}



	if (optind <= 1 || (argc - optind > 0)) {
		print_usage(argc, argv);
		exit(2);
	}


}

static void afc_remove_path_recursive(afc_client_t afc, char* path)
{
    if (!afc || path == NULL)
        return;

    char** list = NULL;
    if (afc_read_directory(afc, path, &list) == AFC_E_SUCCESS) {
        int k;
        for (k = 0; list[k]; k++) {
            if (!strcmp(list[k], ".") || !strcmp(list[k], "..")) {
                continue;
            }

            char **fileinfo = NULL;
            struct stat stbuf;
            uint64_t fblocks = 0;

            // assemble absolute filename
            char *filename = (char*)malloc(strlen(path) + strlen(list[k]) + 1);
            strcpy(filename, path);
            strcat(filename, list[k]);

            // get file information
            afc_get_file_info(afc, filename, &fileinfo);
            if (!fileinfo) {
                continue;
            }

            // parse file information
            int i;
            for (i = 0; fileinfo[i]; i+=2) {
                if (!strcmp(fileinfo[i], "st_ifmt")) {
                    if (!strcmp(fileinfo[i+1], "S_IFREG")) {
                        stbuf.st_mode = S_IFREG;
                    } else if (!strcmp(fileinfo[i+1], "S_IFDIR")) {
                        stbuf.st_mode = S_IFDIR;
                    } else if (!strcmp(fileinfo[i+1], "S_IFLNK")) {
                        stbuf.st_mode = S_IFLNK;
                    } else if (!strcmp(fileinfo[i+1], "S_IFBLK")) {
                        stbuf.st_mode = S_IFBLK;
                    } else if (!strcmp(fileinfo[i+1], "S_IFCHR")) {
                        stbuf.st_mode = S_IFCHR;
                    } else if (!strcmp(fileinfo[i+1], "S_IFIFO")) {
                        stbuf.st_mode = S_IFIFO;
                    } else if (!strcmp(fileinfo[i+1], "S_IFSOCK")) {
                        stbuf.st_mode = S_IFSOCK;
                    }
                }
            }

            // free file information
            afc_dictionary_free(fileinfo);

            if (S_ISDIR(stbuf.st_mode)) {
                // recurse into subdirectories
                char *directoryname = (char*)malloc(strlen(filename) + 2);
                strcpy(directoryname, filename);
                strcat(directoryname, "/");

                afc_remove_path_recursive(afc, directoryname);

                if (directoryname)
                    free(directoryname);

                // remove filesystem entry
                afc_remove_path(afc, filename);
            } else if (S_ISREG(stbuf.st_mode) || S_ISLNK(stbuf.st_mode)) {
                // remove filesystem entry
                afc_remove_path(afc, filename);
            }

            if (filename)
                free(filename);
        }
        afc_dictionary_free(list);
    }
}

JNIEXPORT void JNICALL Java_org_uiautomation_iosdriver_services_DeviceInstallerService_emptyApplicationCacheNative(JNIEnv * env, jobject instance, jstring uuid, jstring bundleIdentifier){
    const char *c_uuid = (*env)->GetStringUTFChars(env, uuid, 0);
    const char *c_bundleIdentifier = (*env)->GetStringUTFChars(env, bundleIdentifier, 0);

    idevice_t device = NULL;
    lockdownd_client_t lockdown = NULL;
    afc_client_t afc = NULL;
    house_arrest_client_t house_arrest = NULL;
    lockdownd_service_descriptor_t service = NULL;
    if (IDEVICE_E_SUCCESS != idevice_new(&device, c_uuid)) {
        throwException(env, "Cannot find device with uuid %s", c_uuid);
        return;
    }


    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(device, &lockdown, "java")) {
        throwException(env, "Could not connect to lockdownd. Exiting.\n");
        goto leave_cleanup;
    }

    if ((lockdownd_start_service
         (lockdown, "com.apple.mobile.house_arrest",
          &service) != LOCKDOWN_E_SUCCESS) || !service) {
        throwException(env, "Could not start com.apple.mobile.notification_proxy!\n");
        goto leave_cleanup;
    }

    if (house_arrest_client_new(device, service, &house_arrest) != HOUSE_ARREST_E_SUCCESS) {
        throwException(env, "Could not create house_arrest service!\n");
        goto leave_cleanup;
    }  

    if (service) {
        lockdownd_service_descriptor_free(service);
        service = NULL;
    }
    // request container access
    house_arrest_error_t res = house_arrest_send_command(house_arrest, "VendDocuments", c_bundleIdentifier);
    if (res != HOUSE_ARREST_E_SUCCESS) {
        throwException(env, "Error %d when trying to get VendDocuments access\n", res);
        goto leave_cleanup;
    }

    plist_t dict = NULL;
    if (house_arrest_get_result(house_arrest, &dict) != HOUSE_ARREST_E_SUCCESS) {
        if (house_arrest_get_result(house_arrest, &dict) != HOUSE_ARREST_E_SUCCESS) {
            throwException(env, "Unable to get result from command.\n");
            goto leave_cleanup;
        }
    }

    plist_t node = plist_dict_get_item(dict, "Error");
    if (node) {
        char *str = NULL;
        plist_get_string_val(node, &str);
        throwException(env, "Error result returned %s\n", str);
        if (str) free(str);
        plist_free(dict);
        dict = NULL;
        goto leave_cleanup;
    }
    node = plist_dict_get_item(dict, "Status");
    if (node) {
        char *str = NULL;
        plist_get_string_val(node, &str);
        if (str && (strcmp(str, "Complete") != 0)) {
            logWarning("Warning: Status is not 'Complete' but '%s'\n", str);
        }
        if (str) free(str);
        plist_free(dict);
        dict = NULL;
    }
    if (dict) {
        plist_free(dict);
    }

    afc_error_t ae = afc_client_new_from_house_arrest_client(house_arrest, &afc);
    if (ae != AFC_E_SUCCESS) {
        throwException(env, "Unable to derieve afc client from house_arrest client due afc error %d\n", ae);
        goto leave_cleanup;
    }
    if (ae == AFC_E_SUCCESS) {
        // remove Documents
        afc_remove_path_recursive(afc, "Documents/");
        // remove Caches
        afc_remove_path_recursive(afc, "Library/Caches/");
        // remove Preferences
        afc_remove_path_recursive(afc, "Library/Preferences/");
        // remove tmp
        afc_remove_path_recursive(afc, "tmp/");

        // recreate minimal directory structure
        afc_make_directory(afc, "Documents");
        afc_make_directory(afc, "Library/Caches");
        afc_make_directory(afc, "Library/Preferences");
        afc_make_directory(afc, "tmp");
    }
    // clean up memory
leave_cleanup:
    if (house_arrest) {
        house_arrest_client_free(house_arrest);
    }
    if (lockdown) {
        lockdownd_client_free(lockdown);
    }
    if (device) {
        idevice_free(device);
    }
}


JNIEXPORT jstring JNICALL Java_org_uiautomation_iosdriver_services_DeviceInstallerService_installNative(JNIEnv * env, jobject instance, jobjectArray stringArray){

   if (jvm == NULL){
   int status = (*env)->GetJavaVM(env, &jvm);
        if(status != 0) {
            logError("failed storing the JVM instance.");
            return;
        }
   }

   int argc = (*env)->GetArrayLength(env, stringArray);
   int i;
   char **argv = (char **)malloc(argc*sizeof(char *));
   for (i=0; i<argc; i++) {
           jstring string = (jstring) (*env)->GetObjectArrayElement(env, stringArray, i);
           const char *rawString = (*env)->GetStringUTFChars(env, string, 0);
           argv[i] = (char*)rawString;
           //printf("[%d]=%s\n",i,argv[i]);
    }
    // to be able to free.
    //char* xml_result;
    idevice_t phone = NULL;
    lockdownd_client_t client = NULL;
    instproxy_client_t ipc = NULL;
    np_client_t np = NULL;
    afc_client_t afc = NULL;
    lockdownd_service_descriptor_t descriptor = NULL;
    int res = 0;
    jstring retval;

    //printf("uninstall_mode=%i\n",uninstall_mode);
    parse_opts(argc, argv);
    //printf("uninstall_mode=%i\n",uninstall_mode);
    argc -= optind;
    argv += optind;


    if (IDEVICE_E_SUCCESS != idevice_new(&phone, uuid)) {
        logError("Cannot find phone with uuid %s",uuid);
        throwException(env, "No iPhone found, is it plugged in?\n");
        return;
    }

    if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(phone, &client, "java")) {
        fprintf(stderr, "Could not connect to lockdownd. Exiting.\n");
        goto leave_cleanup;
    }

    if ((lockdownd_start_service
         (client, "com.apple.mobile.notification_proxy",
          &descriptor) != LOCKDOWN_E_SUCCESS) || !descriptor) {
        fprintf(stderr,
                "Could not start com.apple.mobile.notification_proxy!\n");
        goto leave_cleanup;
    }

    if (np_client_new(phone, descriptor, &np) != NP_E_SUCCESS) {
        fprintf(stderr, "Could not connect to notification_proxy!\n");
        goto leave_cleanup;
    }


    np_set_notify_callback(np, notifier, NULL);

    const char *noties[3] = { NP_APP_INSTALLED, NP_APP_UNINSTALLED, NULL };

    np_observe_notifications(np, noties);

run_again:
    descriptor = NULL;
    if ((lockdownd_start_service
         (client, "com.apple.mobile.installation_proxy",
          &descriptor) != LOCKDOWN_E_SUCCESS) || !descriptor) {
        fprintf(stderr,
                "Could not start com.apple.mobile.installation_proxy!\n");
        goto leave_cleanup;
    }

    if (instproxy_client_new(phone, descriptor, &ipc) != INSTPROXY_E_SUCCESS) {
        fprintf(stderr, "Could not connect to installation_proxy!\n");
        goto leave_cleanup;
    }

    setbuf(stdout, NULL);

    if (last_status) {
        free(last_status);
        last_status = NULL;
    }
    notification_expected = 0;

    if (list_apps_mode) {
        //fprintf(stderr,"list_apps_mode\n");
        int xml_mode = 0;
        plist_t client_opts = instproxy_client_options_new();
        instproxy_client_options_add(client_opts, "ApplicationType", "User", NULL);
        instproxy_error_t err;
        plist_t apps = NULL;

        /* look for options */
        if (options) {
            char *opts = strdup(options);
            char *elem = strtok(opts, ",");
            while (elem) {
                if (!strcmp(elem, "list_system")) {
                    if (!client_opts) {
                        client_opts = instproxy_client_options_new();
                    }
                    instproxy_client_options_add(client_opts, "ApplicationType", "System", NULL);
                } else if (!strcmp(elem, "list_all")) {
                    instproxy_client_options_free(client_opts);
                    client_opts = NULL;
                } else if (!strcmp(elem, "list_user")) {
                    /* do nothing, we're already set */
                } else if (!strcmp(elem, "xml")) {
                    xml_mode = 1;
                }
                elem = strtok(NULL, ",");
            }
        }

        err = instproxy_browse(ipc, client_opts, &apps);
        instproxy_client_options_free(client_opts);
        if (err != INSTPROXY_E_SUCCESS) {
            fprintf(stderr, "ERROR: instproxy_browse returned %d\n", err);
            goto leave_cleanup;
        }
        if (!apps || (plist_get_node_type(apps) != PLIST_ARRAY)) {
            fprintf(stderr,
                    "ERROR: instproxy_browse returnd an invalid plist!\n");
            goto leave_cleanup;
        }
        if (xml_mode) {
            char *xml = NULL;
            uint32_t len = 0;

            plist_to_xml(apps, &xml, &len);
            if (xml) {
                //puts(xml);
                //xml_result=(char*)malloc(strlen(xml)*sizeof(char));
                //strcpy(xml_result,xml);
                retval = (*env)->NewStringUTF(env, xml);
                free(xml);
            }
            plist_free(apps);
            goto leave_cleanup;
        }
        printf("Total: %d apps\n", plist_array_get_size(apps));
        uint32_t i = 0;
        for (i = 0; i < plist_array_get_size(apps); i++) {
            plist_t app = plist_array_get_item(apps, i);
            plist_t p_appid =
                plist_dict_get_item(app, "CFBundleIdentifier");
            char *s_appid = NULL;
            char *s_dispName = NULL;
            char *s_version = NULL;
            plist_t dispName =
                plist_dict_get_item(app, "CFBundleDisplayName");
            plist_t version = plist_dict_get_item(app, "CFBundleVersion");

            if (p_appid) {
                plist_get_string_val(p_appid, &s_appid);
            }
            if (!s_appid) {
                fprintf(stderr, "ERROR: Failed to get APPID!\n");
                break;
            }

            if (dispName) {
                plist_get_string_val(dispName, &s_dispName);
            }
            if (version) {
                plist_get_string_val(version, &s_version);
            }

            if (!s_dispName) {
                s_dispName = strdup(s_appid);
            }
            if (s_version) {
                printf("%s - %s %s\n", s_appid, s_dispName, s_version);
                free(s_version);
            } else {
                printf("%s - %s\n", s_appid, s_dispName);
            }
            free(s_dispName);
            free(s_appid);
        }
        plist_free(apps);
    } else if (install_mode || upgrade_mode) {
        //printf("install_mode || upgrade_mode\n");
        plist_t sinf = NULL;
        plist_t meta = NULL;
        char *pkgname = NULL;
        struct stat fst;
        FILE *f = NULL;
        uint64_t af = 0;
        char buf[8192];

        descriptor = NULL;
        if ((lockdownd_start_service(client, "com.apple.afc", &descriptor) !=
             LOCKDOWN_E_SUCCESS) || !descriptor) {
            fprintf(stderr, "Could not start com.apple.afc!\n");
            goto leave_cleanup;
        }

        lockdownd_client_free(client);
        client = NULL;

        if (afc_client_new(phone, descriptor, &afc) != INSTPROXY_E_SUCCESS) {
            fprintf(stderr, "Could not connect to AFC!\n");
            goto leave_cleanup;
        }

        if (stat(appid, &fst) != 0) {
            fprintf(stderr, "ERROR: stat: %s: %s\n", appid, strerror(errno));
            goto leave_cleanup;
        }

        /* open install package */
        int errp = 0;
        struct zip *zf = zip_open(appid, 0, &errp);
        if (!zf) {
            fprintf(stderr, "ERROR: zip_open: %s: %d\n", appid, errp);
            goto leave_cleanup;
        }

        /* extract iTunesMetadata.plist from package */
        char *zbuf = NULL;
        uint32_t len = 0;
        if (zip_f_get_contents(zf, "iTunesMetadata.plist", 0, &zbuf, &len) == 0) {
            meta = plist_new_data(zbuf, len);
        }
        if (zbuf) {
            free(zbuf);
        }

        /* we need to get the CFBundleName first */
        plist_t info = NULL;
        zbuf = NULL;
        len = 0;
        if (zip_f_get_contents(zf, "Info.plist", ZIP_FL_NODIR, &zbuf, &len) < 0) {
            zip_unchange_all(zf);
            zip_close(zf);
            goto leave_cleanup;
        }
        if (memcmp(zbuf, "bplist00", 8) == 0) {
            plist_from_bin(zbuf, len, &info);
        } else {
            plist_from_xml(zbuf, len, &info);
        }
        free(zbuf);

        if (!info) {
            fprintf(stderr, "Could not parse Info.plist!\n");
            zip_unchange_all(zf);
            zip_close(zf);
            goto leave_cleanup;
        }

        char *bundlename = NULL;

        plist_t bname = plist_dict_get_item(info, "CFBundleName");
        if (bname) {
            plist_get_string_val(bname, &bundlename);
        }
        plist_free(info);

        if (!bundlename) {
            fprintf(stderr, "Could not determine CFBundleName!\n");
            zip_unchange_all(zf);
            zip_close(zf);
            goto leave_cleanup;
        }

        char *sinfname = NULL;
            if (asprintf(&sinfname, "Payload/%s.app/SC_Info/%s.sinf", bundlename, bundlename) < 0) {
            fprintf(stderr, "Out of memory!?\n");
            goto leave_cleanup;
        }
        free(bundlename);

        /* extract .sinf from package */
        zbuf = NULL;
        len = 0;
        if (zip_f_get_contents(zf, sinfname, 0, &zbuf, &len) == 0) {
            sinf = plist_new_data(zbuf, len);
        }
        free(sinfname);
        if (zbuf) {
            free(zbuf);
        }

        zip_unchange_all(zf);
        zip_close(zf);

        /* copy archive to device */
        f = fopen(appid, "r");
        if (!f) {
            fprintf(stderr, "fopen: %s: %s\n", appid, strerror(errno));
            goto leave_cleanup;
        }

        pkgname = NULL;
        if (asprintf(&pkgname, "%s/%s", PKG_PATH, basename(appid)) < 0) {
            fprintf(stderr, "Out of memory!?\n");
            goto leave_cleanup;
        }

        printf("Copying '%s' --> '%s'\n", appid, pkgname);

        char **strs = NULL;
        if (afc_get_file_info(afc, PKG_PATH, &strs) != AFC_E_SUCCESS) {
            if (afc_make_directory(afc, PKG_PATH) != AFC_E_SUCCESS) {
                fprintf(stderr, "WARNING: Could not create directory '%s' on device!\n", PKG_PATH);
            }
        }
        if (strs) {
            int i = 0;
            while (strs[i]) {
                free(strs[i]);
                i++;
            }
            free(strs);
        }

        if ((afc_file_open(afc, pkgname, AFC_FOPEN_WRONLY, &af) !=
             AFC_E_SUCCESS) || !af) {
            fclose(f);
            fprintf(stderr, "afc_file_open on '%s' failed!\n", pkgname);
            free(pkgname);
            goto leave_cleanup;
        }

        size_t amount = 0;
        do {
            amount = fread(buf, 1, sizeof(buf), f);
            if (amount > 0) {
                uint32_t written, total = 0;
                while (total < amount) {
                    written = 0;
                    if (afc_file_write(afc, af, buf, amount, &written) !=
                        AFC_E_SUCCESS) {
                        fprintf(stderr, "AFC Write error!\n");
                        break;
                    }
                    total += written;
                }
                if (total != amount) {
                    fprintf(stderr, "Error: wrote only %d of %zu\n", total,
                            amount);
                    afc_file_close(afc, af);
                    fclose(f);
                    free(pkgname);
                    goto leave_cleanup;
                }
            }
        }
        while (amount > 0);

        afc_file_close(afc, af);
        fclose(f);



        /* perform installation or upgrade */
        plist_t client_opts = instproxy_client_options_new();
        if (sinf) {
            instproxy_client_options_add(client_opts, "ApplicationSINF", sinf, NULL);
        }
        if (meta) {
            instproxy_client_options_add(client_opts, "iTunesMetadata", meta, NULL);
        }
        if (install_mode) {
            printf("Installing '%s'\n", pkgname);
            instproxy_install(ipc, pkgname, client_opts, status_cb, uuid);
        } else {
            printf("Upgrading '%s'\n", pkgname);
            instproxy_upgrade(ipc, pkgname, client_opts, status_cb, uuid);
        }
        instproxy_client_options_free(client_opts);
        free(pkgname);
        wait_for_op_complete = 1;
        notification_expected = 1;
    } else if (uninstall_mode) {
        //printf("uninstall_mode \n");
        instproxy_uninstall(ipc, appid, NULL, status_cb, uuid);
        wait_for_op_complete = 1;
        notification_expected = 1;
    } else if (list_archives_mode) {
        //printf("list_archives_mode \n");
        int xml_mode = 0;
        plist_t dict = NULL;
        plist_t lres = NULL;
        instproxy_error_t err;

        /* look for options */
        if (options) {
            char *opts = strdup(options);
            char *elem = strtok(opts, ",");
            while (elem) {
                if (!strcmp(elem, "xml")) {
                    xml_mode = 1;
                }
                elem = strtok(NULL, ",");
            }
        }

        err = instproxy_lookup_archives(ipc, NULL, &dict);
        if (err != INSTPROXY_E_SUCCESS) {
            fprintf(stderr, "ERROR: lookup_archives returned %d\n", err);
            goto leave_cleanup;
        }
        if (!dict) {
            fprintf(stderr,
                    "ERROR: lookup_archives did not return a plist!?\n");
            goto leave_cleanup;
        }

        lres = plist_dict_get_item(dict, "LookupResult");
        if (!lres || (plist_get_node_type(lres) != PLIST_DICT)) {
            plist_free(dict);
            fprintf(stderr, "ERROR: Could not get dict 'LookupResult'\n");
            goto leave_cleanup;
        }

        if (xml_mode) {
            char *xml = NULL;
            uint32_t len = 0;

            plist_to_xml(lres, &xml, &len);
            if (xml) {
                 //puts(xml);
                 //xml_result=(char*)malloc(strlen(xml)*sizeof(char));
                 //strcpy(xml_result,xml);
                 retval = (*env)->NewStringUTF(env, xml);
                 free(xml);

            }
            plist_free(dict);
            goto leave_cleanup;
        }
        plist_dict_iter iter = NULL;
        plist_t node = NULL;
        char *key = NULL;

        printf("Total: %d archived apps\n", plist_dict_get_size(lres));
        plist_dict_new_iter(lres, &iter);
        if (!iter) {
            plist_free(dict);
            fprintf(stderr, "ERROR: Could not create plist_dict_iter!\n");
            goto leave_cleanup;
        }
        do {
            key = NULL;
            node = NULL;
            plist_dict_next_item(lres, iter, &key, &node);
            if (key && (plist_get_node_type(node) == PLIST_DICT)) {
                char *s_dispName = NULL;
                char *s_version = NULL;
                plist_t dispName =
                    plist_dict_get_item(node, "CFBundleDisplayName");
                plist_t version =
                    plist_dict_get_item(node, "CFBundleVersion");
                if (dispName) {
                    plist_get_string_val(dispName, &s_dispName);
                }
                if (version) {
                    plist_get_string_val(version, &s_version);
                }
                if (!s_dispName) {
                    s_dispName = strdup(key);
                }
                if (s_version) {
                    printf("%s - %s %s\n", key, s_dispName, s_version);
                    free(s_version);
                } else {
                    printf("%s - %s\n", key, s_dispName);
                }
                free(s_dispName);
                free(key);
            }
        }
        while (node);
        plist_free(dict);
    } else if (archive_mode) {
        //printf("archive_mode\n");
        char *copy_path = NULL;
        int remove_after_copy = 0;
        int skip_uninstall = 1;
        int app_only = 0;
        plist_t client_opts = NULL;

        /* look for options */
        if (options) {
            char *opts = strdup(options);
            char *elem = strtok(opts, ",");
            while (elem) {
                if (!strcmp(elem, "uninstall")) {
                    skip_uninstall = 0;
                } else if (!strcmp(elem, "app_only")) {
                    app_only = 1;
                } else if ((strlen(elem) > 5) && !strncmp(elem, "copy=", 5)) {
                    copy_path = strdup(elem+5);
                } else if (!strcmp(elem, "remove")) {
                    remove_after_copy = 1;
                }
                elem = strtok(NULL, ",");
            }
        }

        if (skip_uninstall || app_only) {
            client_opts = instproxy_client_options_new();
            if (skip_uninstall) {
                instproxy_client_options_add(client_opts, "SkipUninstall", 1, NULL);
            }
            if (app_only) {
                instproxy_client_options_add(client_opts, "ArchiveType", "ApplicationOnly", NULL);
            }
        }

        if (copy_path) {
            struct stat fst;
            if (stat(copy_path, &fst) != 0) {
                fprintf(stderr, "ERROR: stat: %s: %s\n", copy_path, strerror(errno));
                free(copy_path);
                goto leave_cleanup;
            }

            if (!S_ISDIR(fst.st_mode)) {
                fprintf(stderr, "ERROR: '%s' is not a directory as expected.\n", copy_path);
                free(copy_path);
                goto leave_cleanup;
            }

            descriptor = 0;
            if ((lockdownd_start_service(client, "com.apple.afc", &descriptor) != LOCKDOWN_E_SUCCESS) || !descriptor) {
                fprintf(stderr, "Could not start com.apple.afc!\n");
                free(copy_path);
                goto leave_cleanup;
            }

            lockdownd_client_free(client);
            client = NULL;

            if (afc_client_new(phone, descriptor, &afc) != INSTPROXY_E_SUCCESS) {
                fprintf(stderr, "Could not connect to AFC!\n");
                goto leave_cleanup;
            }
        }

        instproxy_archive(ipc, appid, client_opts, status_cb, uuid);
        instproxy_client_options_free(client_opts);
        wait_for_op_complete = 1;
        if (skip_uninstall) {
            notification_expected = 0;
        } else {
            notification_expected = 1;
        }

        do_wait_when_needed();

        if (copy_path) {
            if (err_occured) {
                afc_client_free(afc);
                afc = NULL;
                goto leave_cleanup;
            }
            FILE *f = NULL;
            uint64_t af = 0;
            /* local filename */
            char *localfile = NULL;
            if (asprintf(&localfile, "%s/%s.ipa", copy_path, appid) < 0) {
                fprintf(stderr, "Out of memory!?\n");
                goto leave_cleanup;
            }
            free(copy_path);

            f = fopen(localfile, "w");
            if (!f) {
                fprintf(stderr, "ERROR: fopen: %s: %s\n", localfile, strerror(errno));
                free(localfile);
                goto leave_cleanup;
            }

            /* remote filename */
            char *remotefile = NULL;
            if (asprintf(&remotefile, "%s/%s.zip", APPARCH_PATH, appid) < 0) {
                fprintf(stderr, "Out of memory!?\n");
                goto leave_cleanup;
            }

            uint32_t fsize = 0;
            char **fileinfo = NULL;
            if ((afc_get_file_info(afc, remotefile, &fileinfo) != AFC_E_SUCCESS) || !fileinfo) {
                printf("afc_get: %d",(afc_get_file_info(afc, remotefile, &fileinfo)));
                if (!fileinfo){
                printf("! fileinfo");
                }
                fprintf(stderr, "ERROR getting AFC file info for '%s' on device!\n", remotefile);
                fclose(f);
                free(remotefile);
                free(localfile);
                goto leave_cleanup;
            }

            int i;
            for (i = 0; fileinfo[i]; i+=2) {
                if (!strcmp(fileinfo[i], "st_size")) {
                    fsize = atoi(fileinfo[i+1]);
                    break;
                }
            }
            i = 0;
            while (fileinfo[i]) {
                free(fileinfo[i]);
                i++;
            }
            free(fileinfo);

            if (fsize == 0) {
                fprintf(stderr, "Hm... remote file length could not be determined. Cannot copy.\n");
                fclose(f);
                free(remotefile);
                free(localfile);
                goto leave_cleanup;
            }

            if ((afc_file_open(afc, remotefile, AFC_FOPEN_RDONLY, &af) != AFC_E_SUCCESS) || !af) {
                fclose(f);
                fprintf(stderr, "ERROR: could not open '%s' on device for reading!\n", remotefile);
                free(remotefile);
                free(localfile);
                goto leave_cleanup;
            }

            /* copy file over */
            //printf("Copying '%s' --> '%s'\n", remotefile, localfile);
            logInfo("ArchiveCopy - '%s' --> '%s'\n", remotefile, localfile);
            free(remotefile);
            free(localfile);

            uint32_t amount = 0;
            uint32_t total = 0;
            char buf[8192];

            do {
                if (afc_file_read(afc, af, buf, sizeof(buf), &amount) != AFC_E_SUCCESS) {
                    fprintf(stderr, "AFC Read error!\n");
                    break;
                }

                if (amount > 0) {
                    size_t written = fwrite(buf, 1, amount, f);
                    if (written != amount) {
                        fprintf(stderr, "Error when writing %d bytes to local file!\n", amount);
                        break;
                    }
                    total += written;
                }
            } while (amount > 0);

            afc_file_close(afc, af);
            fclose(f);

            if (total != fsize) {
                fprintf(stderr, "WARNING: remote and local file sizes don't match (%d != %d)\n", fsize, total);
                if (remove_after_copy) {
                    fprintf(stderr, "NOTE: archive file will NOT be removed from device\n");
                    remove_after_copy = 0;
                }
            }

            if (remove_after_copy) {
                /* remove archive if requested */
                printf("Removing '%s'\n", appid);
                archive_mode = 0;
                remove_archive_mode = 1;
                free(options);
                options = NULL;
                if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(phone, &client, "java")) {
                    fprintf(stderr, "Could not connect to lockdownd. Exiting.\n");
                    goto leave_cleanup;
                }
                goto run_again;
            }
        }
        goto leave_cleanup;
    } else if (restore_mode) {
        //printf("restore_mode\n");
        instproxy_restore(ipc, appid, NULL, status_cb, uuid);
        wait_for_op_complete = 1;
        notification_expected = 1;
    } else if (remove_archive_mode) {
        //printf("remove_archive_mode\n");
        instproxy_remove_archive(ipc, appid, NULL, status_cb, uuid);
        wait_for_op_complete = 1;
    } else {
        printf
            ("ERROR: no operation selected?! This should not be reached!\n");
        res = -2;
        goto leave_cleanup;
    }

    if (client) {
        /* not needed anymore */
        lockdownd_client_free(client);
        client = NULL;
    }

    do_wait_when_needed();

  leave_cleanup:
    if (np) {
        np_client_free(np);
    }
    if (ipc) {
        instproxy_client_free(ipc);
    }
    if (afc) {
        afc_client_free(afc);
        afc = NULL;
    }
    if (client) {
        lockdownd_client_free(client);
        client = NULL;
    }
    idevice_free(phone);

    if (uuid) {
        free(uuid);
        uuid = NULL;
    }
    if (appid) {
        free(appid);
        appid = NULL;
    }
    if (options) {
    //    free(options);
    }
    //printf("returning with xml:\n\n  %s",xml_result);
    return retval;

}