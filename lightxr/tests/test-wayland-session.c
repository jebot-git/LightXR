/* Exercise the real daemon session objects and re-exec worker protocol without PAM or a VT switch. */
#include "session.h"
#include "wayland-session.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static void read_exact(int fd,void* data,size_t count){size_t pos=0;while(pos<count){ssize_t n=read(fd,(char*)data+pos,count-pos);if(n<=0)_exit(20);pos+=n;}}
static gchar* read_string(int fd){int n;read_exact(fd,&n,sizeof(n));if(n<0)return NULL;if(n>65535)_exit(21);gchar* s=g_malloc(n+1);read_exact(fd,s,n);s[n]=0;return s;}
static gboolean stopped=FALSE;
static void on_stopped(Session* session,gpointer data){(void)session;(void)data;stopped=TRUE;}
int main(int argc,char** argv){
    if(argc==4&&!strcmp(argv[1],"--session-child")){
        int fd=atoi(argv[2]),version,authenticate,interactive;read_exact(fd,&version,sizeof(version));
        g_autofree gchar* service=read_string(fd);g_autofree gchar* user=read_string(fd);
        read_exact(fd,&authenticate,sizeof(authenticate));read_exact(fd,&interactive,sizeof(interactive));
        g_autofree gchar* legacy=read_string(fd);g_autofree gchar* tty=read_string(fd);
        g_autofree gchar* remote=read_string(fd);g_autofree gchar* xdisplay=read_string(fd);g_autofree gchar* xauth=read_string(fd);
        if(xdisplay||xauth)_exit(23);
        /* Report only test fixture metadata, never credentials. */
        const gchar* report=g_getenv("LIGHTXR_WORKER_TEST_REPORT");
        gchar* content=g_strdup_printf("%s\n%s\n%s\n",service,user,tty);
        gboolean result=g_file_set_contents(report,content,-1,NULL);g_free(content);return result?0:22;
    }
    if(argc!=2||!g_file_test(argv[1],G_FILE_TEST_IS_REGULAR))return 77;
    g_autoptr(GError) error=NULL;
    g_autoptr(SessionConfig) config=session_config_new_from_file(argv[1],"wayland",&error);
    g_assert_no_error(error);g_assert_nonnull(config);g_assert_cmpstr(session_config_get_session_type(config),==,"wayland");
    g_assert_cmpstr(session_config_get_command(config),==,"/usr/local/bin/dome-session");
    g_autoptr(Session) session=session_new();
    session_set_config(session,config);session_set_username(session,"fixture-dome-user");session_set_pam_service(session,"fixture-lightxr");
    session_set_xdisplay(session,":old-greeter");session_set_env(session,"DISPLAY",":old-greeter");session_set_env(session,"WAYLAND_DISPLAY","old-wayland");session_set_env(session,"WAYLAND_SOCKET","9");session_set_env(session,"XAUTHORITY","old-auth");
    g_autoptr(WaylandSession) display=wayland_session_new();wayland_session_set_vt(display,7);g_assert_cmpstr(display_server_get_session_type(DISPLAY_SERVER(display)),==,"wayland");display_server_connect_session(DISPLAY_SERVER(display),session);
    g_assert_cmpstr(session_get_env(session,"XDG_SESSION_TYPE"),==,"wayland");g_assert_cmpstr(session_get_env(session,"XDG_VTNR"),==,"7");
    g_assert_null(session_get_env(session,"DISPLAY"));g_assert_null(session_get_env(session,"WAYLAND_DISPLAY"));g_assert_null(session_get_env(session,"WAYLAND_SOCKET"));g_assert_null(session_get_env(session,"XAUTHORITY"));
    g_autofree gchar* report=g_strdup("/tmp/lightxr-worker-XXXXXX");int fd=g_mkstemp(report);g_assert_cmpint(fd,>=,0);close(fd);g_unlink(report);
    g_setenv("LIGHTXR_WORKER_TEST_REPORT",report,TRUE);g_signal_connect(session,"stopped",G_CALLBACK(on_stopped),NULL);
    g_assert_true(session_start(session));
    gint64 deadline=g_get_monotonic_time()+5*G_TIME_SPAN_SECOND;
    while(!stopped&&g_get_monotonic_time()<deadline){while(g_main_context_iteration(NULL,FALSE));g_usleep(1000);}
    g_assert_true(stopped);g_autofree gchar* content=NULL;g_assert_true(g_file_get_contents(report,&content,NULL,&error));g_assert_no_error(error);
    g_assert_cmpstr(content,==,"fixture-lightxr\nfixture-dome-user\n/dev/tty7\n");g_unlink(report);
    display_server_disconnect_session(DISPLAY_SERVER(display),session);g_assert_null(session_get_env(session,"XDG_VTNR"));
    puts("PASS: installed DomeD Wayland entry, clean display environment, Linux PAM_TTY, VT metadata and trusted session worker re-exec");return 0;
}
