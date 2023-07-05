/*
	Swami Velamala 
	C proxy server
*/
#include <stdio.h>
#include <string.h>	//strlen
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>	//inet_addr
#include <unistd.h>	//write
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>


#define BUFSIZE 1024
#define THREADS 1000


//===========================================================================
int fsize(FILE* fp){
	fseek(fp, 0, SEEK_END); // seek to end of file
	int size = ftell(fp); // get current file pointer
	fseek(fp, 0, SEEK_SET); // seek back to beginning of fi
	return size;
}

unsigned long hash(unsigned char *str)
{
    unsigned long hash = 5381;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

int malformed(char* request){
	char c;
	int space_count = 0;
	for(int i = 0; i < strlen(request); i++){
		c = request[i];
		if(c == '\r'){
			break;
		}
		if(c == ' '){
			space_count ++;
			if(space_count > 2){return -1;}
		}
	}
    if(space_count < 2){return -1;}
	return 0;
}

int find_header_length(char* http_response){
    for(int i =0; i < strlen(http_response); i++){
        if(http_response[i] == '\n' && http_response[i-1] == '\r' && 
           http_response[i-2] == '\n' && http_response[i-3] == '\r' ){
        return i + 1;
        }
    }
}


int find_content_length(char *http_response){
    char response_cp[5000];
    strcpy(response_cp,http_response);
    char * tok1;
    char tok1cp[100];
    char * tok2;
    strtok(response_cp,"\r\n");
    while(1){
		tok1 = strtok(NULL,"\r\n");
        if(tok1 == NULL){return 0;}
        if(tok1[0] == 'C' && tok1[13] == 'h'){
            tok1 = strtok(tok1," ");
            tok1 = strtok(NULL,"\r\n");
            //printf("length: %s\n",tok1);
            return atoi(tok1);
        }
	}

}

int extract_host_from_url(char * url, char * host){
    int slashfound = 0;
    for(int i = 0; i< strlen(url); i++){
        if(slashfound){
            if(url[i] == '/'){break;}
            host[i-slashfound-1] = url[i];
        }
        if( url[i]== '/' && url[i-1]== '/'){slashfound = i;}
    }
    return 0;
}

int insert_host_header(char* request, char* host){
    char host_header[1000] = "\r\nHost: ";
    char* location = strstr(request,"\r\n\r\n");
    strcat(host_header,host);
    memmove(location+strlen(host_header),location,4);
    memcpy(location,host_header,strlen(host_header));
    return 1;
}

int parse_request(char * request, char * method, char * url,
 char * version,char* host, char * errormsg,int host_flag){

    if(malformed(request) == -1){ // need to implement malformed error
        strcat(errormsg,"400 bad request\r\n\r\n");
        return -1;
    }

     if(host_flag){   // if the host header is present go ahead and get the host
        char * loc = strstr(request,"Host: ") + 6;
        char * end = strstr(loc,"\r");
        int len = end - loc;
        memcpy(host,loc,len);

    }

    strcat(method,strtok(request," "));
    strcat(url,strtok(NULL," "));
    strcat(version,strtok(NULL,"\r\n"));

    // check if message is correct

    // check if HTTP Version is supported
    if(strcmp(version,"HTTP/1.0") != 0 && strcmp(version,"HTTP/1.1") != 0){
		strcat(errormsg,"505 HTTP Version Not Supported\r\n\r\n");
		return 505;
	}

    // check that request type is GET if not create error response 
	if(strcmp(method,"GET") != 0){
        strcat(errormsg,"405 Method Not Allowed\r\n\r\n");
		return 405;
	}     

}

int check_server_exists(char * host){  

    struct addrinfo hints, *servinfo, *p;
    int rv;
    memset(&hints, 0, sizeof hints);
    if ((rv = getaddrinfo(host, "http", &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    freeaddrinfo(servinfo);
}

int check_blocklist(char * host){
    char buf[255];
    FILE * fp;
    fp = fopen("blocklist","r");
    while (fgets(buf, 255, fp) != NULL) {
        for(int i = 0; i<strlen(buf); i++){
            if(buf[i] == '\n'){
                buf[i] = '\0';
            }
        }
        if(strcmp(buf,host) == 0){
            fclose(fp);
            return -1;
        }
        bzero(buf,255);
    }
    fclose(fp);
}

int create_server_socket(char* hostname){
    struct hostent* hostinfo;
    struct sockaddr_in server_addr;
    int sockfd;
    hostinfo = gethostbyname(hostname);
    if(hostinfo == NULL){
        printf("gethostbyname faile on :%s\n",hostname);
        return -1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Error: cannot create socket\n");
        exit(1);
    }

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(80);
    bcopy((char *)hostinfo->h_addr, (char *)&server_addr.sin_addr.s_addr, hostinfo->h_length);

    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        printf("Error: cannot connect to server\n");
        return -1;
    }

    return sockfd;

}

int send_cached_file(int client_sock, char * path){
    //printf("found %s in the cache. serving back to client", hashstr);

    struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, 0};

    struct stat st;
    stat(path,&st);
    int fd = open(path,O_RDONLY);
    off_t offset = 0;

    fcntl(fd, F_SETLKW, &lock);
    sendfile(client_sock,fd, &offset,st.st_size);
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLKW, &lock);
    close(fd);

}


int make_server_request(int client_sock, char* request,char* host, char* path){
    
    char buf[5000];
    int server_sock, sent, bytes_received,total_len,bytes_left,header_len,fd;
    int cont_len = 0;
    int total_bytes_rec = 0;
    struct flock lock;

    lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = 0; lock.l_len = 0;

    //FILE* fp = fopen(path,"r");
    //fclose(fp);
    //if (access(path, F_OK) == 0) { printf("caught\n");return 0; }
    fd = open(path,O_WRONLY|O_CREAT, 0666);  // open new file
    if(fcntl(fd, F_SETLK, &lock) == 0){       // try to acquire lock
        printf("%d making serer request\n",getpid());

        // create new server socket and send request to server
        server_sock = create_server_socket(host);
        if(server_sock == -1){printf("failed!!!");return -1;}
        send(server_sock,request,strlen(request),0);

        bytes_received = recv(server_sock,buf,5000,0);
        cont_len = find_content_length(buf);
        header_len = find_header_length(buf);
        total_len = cont_len + header_len;
        bytes_left = total_len - bytes_received ;

        write(fd,buf,bytes_received);
        bzero(buf,5000);

        //printf("%d|path: %s |total len: %d |headerlen: %d |contentlen: %d\n",getpid(),path, total_len,header_len,cont_len);

        while(bytes_left > 0){
            bytes_received = recv(server_sock,buf,5000,0);
            write(fd,buf,bytes_received);
            bytes_left = bytes_left - bytes_received;
            bzero(buf,5000);
        }
        close(server_sock);

        lock.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &lock);
    }else{
        printf("%d serving from cache\n",getpid());
    }
    close(fd);

}

int dynamic_server_request(int client_sock, char* request,char* host){
    
    char buf[5000];
    int server_sock, sent, bytes_received,total_len,bytes_left,header_len,fd;
    int cont_len = 0;
    int total_bytes_rec = 0;
  
    // create new server socket and send request to server
    server_sock = create_server_socket(host);
    if(server_sock == -1){printf("failed!!!");return -1;}
    send(server_sock,request,strlen(request),0);

    bytes_received = recv(server_sock,buf,5000,0);
    cont_len = find_content_length(buf);
    header_len = find_header_length(buf);
    total_len = cont_len + header_len;
    bytes_left = total_len - bytes_received ;

    send(client_sock,buf,bytes_received,0);
    bzero(buf,5000);

    //printf("%d|path: %s |total len: %d |headerlen: %d |contentlen: %d\n",getpid(),path, total_len,header_len,cont_len);

    while(bytes_left > 0){
        bytes_received = recv(server_sock,buf,5000,0);
        send(client_sock,buf,bytes_received,0);
        bytes_left = bytes_left - bytes_received;
        bzero(buf,5000);
    }
    close(server_sock);
}


//===========================================================================
int main(int argc , char *argv[]){
	if(argc < 3){
		printf("not enough args\n");
		return -1;
	}

	int port = atoi(argv[1]);
	if(port <= 1024){
		printf("invalid port #\n");
		return -1;
	}

    int timeout = atoi(argv[2]);
    if(timeout < 1){
        printf("invalid timeout length/n");
        return -1;
    }
    // create cache folder
    struct stat st = {0};
    if (stat("cache", &st) == -1) {
        if(mkdir("cache", 0700) == -1){
            printf("failed to make dir\n");
            return -1;
        }
    }else{
        system("rm cache/*");
    }

    int socket_desc , client_sock , c , read_size;
	struct sockaddr_in server , client;
    int parentid = getpid(); 
   
	//Create socket
	socket_desc = socket(AF_INET , SOCK_STREAM , 0);
	if (socket_desc == -1)
	{
		printf("Could not create socket");
	}
	puts("Socket created");

	//Prepare the sockaddr_in structure
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(port);
    int a = 1;
	setsockopt(socket_desc, SOL_SOCKET,SO_REUSEADDR, &a, sizeof(int));

	//Bind
	if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
	{
		//print the error message
		perror("bind failed. Error");
		return 1;
	}
	puts("bind done");
	
	//Listen
	listen(socket_desc , 3);
    while(1){
        //accept connection from an incoming client
        //puts("Waiting for incoming connections...");
		c = sizeof(struct sockaddr_in);

		client_sock = accept(socket_desc, (struct sockaddr *)&client, (socklen_t*)&c);
		if (client_sock < 0)
		{
			perror("accept failed");
			return 1;
		}

        fork();
            if(getpid() == parentid){
                close(client_sock);
                continue;}
            //printf("Connection accepted pid:%d \n" , getpid());

            /*struct timeval sock_timeout;
            sock_timeout.tv_sec = 1;
            sock_timeout.tv_usec = 0;
            if (setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout)) < 0) {
                perror("setsockopt failed");
                exit(EXIT_FAILURE);
            }*/

        
            char request[5000];   bzero(request,5000);
            char requestcp[5000]; bzero(requestcp,5000);
            //char * requestcp; requestcp = (char*) malloc(5000);


            if((read_size = recv(client_sock ,request , 5000 , 0)) < 0){
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    printf("%d|Timeout occurred! closing proc \n", getpid());
                } else {
                    perror("recv failed");
                }   
                break;
            }

            strcpy(requestcp,request);    
            char * method;   method = (char*) malloc(10);    bzero(method,10);
            char * url;      url = (char*) malloc(1000);     bzero(url,1000);
            char * version;  version = (char*) malloc(10);   bzero(version,10); 
            char * host;     host =  (char*) malloc(100);    bzero(host,100);
            char * errormsg; errormsg = (char*) malloc(100); bzero(errormsg,10);
            int dynamic_flag = 0;

            // 1. Check that the HTTP method is valid (only support get)
            int res;
            if(strstr(requestcp,"Host: ")){     // check if the host header is provided 
                res = parse_request(requestcp,method,url,version,host,errormsg,1);
            }else{                              // if not provided parse and add to request
                res = parse_request(requestcp,method,url,version,host,errormsg,0);
                extract_host_from_url(url,host);
                insert_host_header(request,host);
            }

            if(res != 0){
                    printf("%s\n",errormsg);
                    send(client_sock,errormsg,strlen(errormsg),0);
                    close(client_sock);
                    return 0;
            }


            //Check that the HTTP server specified by URL exists
            if(check_server_exists(host) == -1){
                printf("server doesn't exist\n");
                send(client_sock,"404 Not Found\r\n\r\n",18,0);
                //close(client_sock);
            }

            //Check if the host is in the blocklist
            if(check_blocklist(host) == -1){
                printf("%s BLOCKLISTED\n",host);
                send(client_sock,"403 Forbidden\r\n\r\n",18,0);
                close(client_sock);
                return 0;
            }

            // 3. Check that 1. and 2. pass if not return a 400 BAD request 
            //printf("%d|%s|%s|%s|%s|\n",getpid(),method,url,version,host);

            // check if the page is cached 
            char hashstr[257]; bzero(hashstr,257);
            char path[300];    bzero(path,300); strcat(path,"cache/");
            sprintf(hashstr, "%lu", hash(url));
            strcat(path,hashstr);

            printf("%d|%s|%s|%s|%s|\n",getpid(),method,url,version,host);
            //check if the url is for dynamic content
            if(strstr(url,"?")){
                printf("dynamic content\n");
                dynamic_server_request(client_sock,request,host);

            }else{
                // if the file is in the cache and rea
                if (access(path, F_OK) == 0) {
                    // check if the cached file has expired 
                    struct stat st;
                    stat(path,&st);
                    time_t now = time(NULL);
                    time_t age = now - st.st_mtime;
                    if(age > timeout){
                        make_server_request(client_sock,request,host,path);
                    }else{
                        printf("%d serving from cache\n",getpid());
                    }
                    
                }else{    // if not cached open socket and forward request
                    make_server_request(client_sock,request,host,path);
                }
                send_cached_file(client_sock,path);
            }
            free(method);
            free(url);
            free(version);
            free(errormsg);
        
            close(client_sock);
            exit(0);

    }
    
}


                // TODO 
                // blocklist
                // do not check cache for dynamic content


                //http://neverssl.com/
                //http://netsys.cs.colorado.edu
                //http://oldyoungbrightmagic.neverssl.com/online/
        