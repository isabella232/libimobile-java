#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


static JavaVM *jvm;



void logJava(int level,const char *msg){

  // should come from the context and be set in the service contructor.
  char * logId="[uniqueLogTODO]";
  char * new_str ;
  if((new_str = malloc(strlen(msg)+strlen(logId)+1)) != NULL){
      new_str[0] = '\0';   // ensures the memory is an empty string
      strcat(new_str,logId);
      strcat(new_str,msg);
  }

  JNIEnv *env;
  if (jvm ==NULL){
     printf("Initialize by calling LogService.disabledebug() first..\n");
  }
  (*jvm)->AttachCurrentThread(jvm, (void **)&env, NULL);

  if (env==NULL){
    printf("Need to specific the JNI env for logging.\n");
  }
  char * n = "org/uiautomation/iosdriver/services/LoggerService";
  jclass clazz = (*env)->FindClass(env, n);
  if (clazz==NULL){
          printf("%s can't be loaded in C. Class got renamed ?",n);
          return;
  }
  jmethodID mLog = (*env)->GetStaticMethodID(env,clazz, "log","(ILjava/lang/String;)V");
  if (mLog==NULL){
        printf("mLog NULL");
        return;
  }
  jstring s = (*env)->NewStringUTF(env, msg);
  (*env)->CallStaticVoidMethod(env,clazz, mLog,level,s);
  free((char*)msg);
}
// 0 fine
// 1 info
// 2 warning
// 3 error

void logFine(const char *format, ...){
  va_list args;
  char *msg = NULL;

  va_start(args, format);
  (void)vasprintf(&msg, format, args);
  logJava(0,msg);
  va_end(args);
}
void logInfo(const char *format, ...){
  va_list args;
  char *msg = NULL;

  va_start(args, format);
  (void)vasprintf(&msg, format, args);
  logJava(1,msg);
  va_end(args);
}
void logWarning(const char *format, ...){
  va_list args;
  char *msg = NULL;

  va_start(args, format);
  (void)vasprintf(&msg, format, args);
  logJava(2,msg);
  va_end(args);
}
void logError(const char *format, ...){
  va_list args;
  char *msg = NULL;

  va_start(args, format);
  (void)vasprintf(&msg, format, args);
  logJava(3,msg);
  va_end(args);
}

JNIEXPORT void JNICALL Java_org_uiautomation_iosdriver_services_LoggerService_setLogLevel(JNIEnv * env, jclass clazz, jint level){
 idevice_set_debug_level((int)level);
 int status = (*env)->GetJavaVM(env, &jvm);
     if(status != 0) {
         printf("failed storing the JVM instance.");
     }
 logInfo("Yop");
}

