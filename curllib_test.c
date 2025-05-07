#include "curllib.h"
#include <stdio.h>
#include <time.h>

void send_msg_server (const char * msg)
{

}


const char* GetIP()
{
  FILE *fp;
  static char line[256];  // 적절한 버퍼 크기를 선택합니다.

  fp = fopen("local_ip.log", "r+t");
  if(NULL == fp){
    printf ("can not get local ip log\n");
    return NULL;
  }
  
  fgets(line, sizeof(line), fp);
  fgets(line, sizeof(line), fp);
  fclose(fp);
  return line;
}


int main (int argc, char *argv[])
{
    CurlIinfoType j;

    strcpy(j.phone, "01027061463");
    strcpy(j.password, "12341234");
    strcpy(j.server_ip, "52.194.238.184");
    
    j.port = 0;

    strcpy(j.snapshot_path, "/home/nvidia/webrtc/cam0_snapshot.jpg");
    sprintf(j.video_url, "http://121.67.120.204:12345/data/event/event_01.webm");
    printf("video url %s \n", j.video_url);


    strcpy(j.token, "b840c40720168192336a0df4fb1d710cf07d67cd");
    if(login_request(&j) == 0){
        notification_request("ITC100A-23081012","1", &j);
    }

}
