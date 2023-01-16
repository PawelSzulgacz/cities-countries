#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <netdb.h>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <signal.h>
#include <sys/epoll.h>
#include <iostream>
#include <chrono>
#include <ctime>
#include <cstring>
#include <string>


using namespace std;

const size_t rounds = 5;
int time_for_round = 120;
int time_after_first = 30;
char letters[23] = {'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','r','s','t','u','w','z'};
string names[1000] = {"Marzena","Milena","Mortadela","Mirosław"};


int servFd;
// client sockets
std::mutex clientFdsLock;
std::unordered_set<int> clientFds;


// handles SIGINT
void ctrl_c(int);
// sends data to clientFds excluding fd
void sendToAll(char * buffer, int count);
// converts cstring to port
uint16_t readPort(char * txt);
// sets SO_REUSEADDR
void setReuseAddr(int sock);


static void epoll_add(int myepoll, int fd, uint32_t events)
{
	struct epoll_event event;
	event.events = events;
	event.data.fd = fd;
	if (epoll_ctl(myepoll, EPOLL_CTL_ADD, fd, &event) == -1) {
		perror("epoll_ctl()\n");
		exit(1);
	}
}

uint16_t readPort(char * txt){
    char * ptr;
    auto port = strtol(txt, &ptr, 10);
    if(*ptr!=0 || port<1 || (port>((1<<16)-1))) error(1,0,"illegal argument %s", txt);
    return port;
}

void setReuseAddr(int sock){
    const int one = 1;
    int res = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(res) error(1,errno, "setsockopt failed");
}

void ctrl_c(int){
    std::unique_lock<std::mutex> lock(clientFdsLock);
    for(int clientFd : clientFds){
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
    }
    close(servFd);
    printf("Closing server\n");
    exit(0);
}

void sendToAll(char * buffer, int count){
    int res;
    std::unique_lock<std::mutex> lock(clientFdsLock);
    //cout<<endl<<"buffer----"<<buffer<<"----count "<<count<<endl;
    for(int clientFd : clientFds){
        res = send(clientFd, buffer, count, MSG_DONTWAIT);
    }
}

void random_letters(char* lett)
{
    srand(time(NULL));
    int r;
    char c;
    for(int i = 0;i < rounds; i++)
    {
        int r = rand()%22;
        lett[i] = letters[r];
        for(int x = 0 ;x < i; x++)
            {
            if (lett[x] == lett[i])
            i--;
            }
    }
}

void countdown(int t)
{
    for (int i = t; i > 0; i--) {
        
        if(clientFds.size() <= 1) break;
        std::cout << std::endl<<i;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

string filler(int x)
{
    string fill = to_string(x);
    for(int i = fill.size(); i < 5 ;i++)
        fill.insert(0,"0");
    return fill;
}

void game(){
    char round_letters[rounds]; 
    string message;
    string helper;
    int sizer;
    char buffer[255];
    helper = "abcdefghjiklmnoprs1111222233334444\0aaaa";
    message.append(helper);
    while(true)
    {
        if(clientFds.size() > 1)
            {
                random_letters(round_letters);
                cout << round_letters;
                //helper = round_letters[0];

                sizer = message.size();
                message.insert(0,filler(sizer));

                strcpy(buffer, message.c_str());
                cout<<buffer;
                sendToAll(buffer,strlen(buffer));
                countdown(10);
                sendToAll(buffer,strlen(buffer));
                countdown(100);
            }
    }
}


int main(int argc, char ** argv)
{



    if(argc != 2) error(1, 0, "Need 1 arg (port)");
    auto port = readPort(argv[1]);
    
    // create socket
    servFd = socket(AF_INET, SOCK_STREAM, 0);
    if(servFd == -1) error(1, errno, "socket failed");
    
    // graceful ctrl+c exit
    signal(SIGINT, ctrl_c);
    // prevent dead sockets from raising pipe signals on write
    signal(SIGPIPE, SIG_IGN);
    
    //pozwala nam uzywac tego samego adresu kilka razy, ale działa bez tegowiec nw
    setReuseAddr(servFd);

    // bind to any address and port provided in arguments
    sockaddr_in serverAddr{.sin_family=AF_INET, .sin_port=htons((short)port), .sin_addr={INADDR_ANY}};
    int res = bind(servFd, (sockaddr*) &serverAddr, sizeof(serverAddr));
    if(res) error(1, errno, "bind failed");
    
    // enter listening mode
    res = listen(servFd, 5);
    if(res) error(1, errno, "listen failed");


	int myepoll = epoll_create1(0);
	
	if (myepoll== -1) {
		fprintf(stderr, "Failed to create epoll file descriptor\n");
		return 1;
	}

    std::thread first (game);

    struct epoll_event events[32];

    epoll_add(myepoll, servFd, EPOLLIN | EPOLLOUT | EPOLLET);



    while(true){
        int waiter = epoll_wait(myepoll, events, 32, -1);
        for(int i = 0;i < waiter; i++)
        {

            if(events[i].data.fd == servFd)
            {

                sockaddr_in clientAddr{};
                socklen_t clientAddrSize = sizeof(clientAddr);
                auto clientFd = accept(servFd, (sockaddr*) &clientAddr, &clientAddrSize);
                if(clientFd == -1) error(1, errno, "accept failed");
                
                epoll_add(myepoll, clientFd, EPOLLIN | EPOLLET | EPOLLRDHUP |EPOLLHUP);
                

                {
                std::unique_lock<std::mutex> lock(clientFdsLock);
                clientFds.insert(clientFd);
                }

                printf("new connection from: %s:%hu (fd: %d)\n ", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port), clientFd);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP)){
                printf("[+] connection closed\n");
                epoll_ctl(myepoll, EPOLL_CTL_DEL,events[i].data.fd, NULL);
                close(events[i].data.fd);
                clientFds.erase(events[i].data.fd);
            continue;
            }
            else if(events[i].events & EPOLLIN)
            {
                char buf[512];
                int n = read(events[i].data.fd, buf, 512);
                printf("[+] data: %s\n", buf);
            }


        }

        }


	return 0;
}


