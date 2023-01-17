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
#include <fstream>
#include <algorithm>
#include <cctype>

using namespace std;
//liczba rund, czas na runde, czas na runde gdy 1 wysle hasla,litery do losowania, maksymalna liczba klientow
const size_t rounds = 5;
int time_for_round = 120;
int time_after_first = 30;
char letters[23] = {'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','r','s','t','u','w','z'};
int max_clients = 1000;


int servFd;
// client sockets
std::mutex clientFdsLock;
std::unordered_set<int> clientFds;


//struktura klienta, jego fd, imie, punkty, ostatnio zgadniete hasla
struct Client{
    int fd;
    string name;
    int points = 0;
    string given[4];
};
//tablica klientow, liczba klientow w tym momencie, zmienna inicjalizujaca stop gry, gdy jest za malo klientow, klienci w danej rundzie, odpowiedzi klientow w danej rundzie
Client clients[1000];
int numClients = 0;
int stopper = 0;
int roundClients = 0;
int roundAnswers = 0;

//lista zwierzat,miast,imion,krajow
string animals[202];
string cities[954];
string polNames[436];
string countries[417];
int sizeAnimals;
int sizeCities;
int sizePolNames;
int sizeCountries;


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

//bierze dane z plikow
void getData()
{
    int i = 0;
    string word;

    ifstream f_countries("data/countries.txt");
    while(getline(f_countries, word))
    {   
        std::transform(word.begin(), word.end(), word.begin(),[](unsigned char c){ return std::tolower(c); });
        countries[i] = word;
        i++;
    }
    f_countries.close();
    sizeCountries = i;
    i = 0;
    ifstream f_names("data/imiona.txt");
    while(getline(f_names, word))
    {   
        std::transform(word.begin(), word.end(), word.begin(),[](unsigned char c){ return std::tolower(c); });
        polNames[i] = word;
        i++;
    }
    f_names.close();
    sizePolNames = i;
    i = 0;
    ifstream f_cities("data/Polskie_Miasta.txt");
    while(getline(f_cities, word))
    {   
        std::transform(word.begin(), word.end(), word.begin(),[](unsigned char c){ return std::tolower(c); });
        cities[i] = word;
        i++;
    }
    f_cities.close();
    sizeCities = i;
    i = 0;
    ifstream f_animals("data/zwierzeta.txt");
    while(getline(f_animals, word))
    {   
        std::transform(word.begin(), word.end(), word.begin(),[](unsigned char c){ return std::tolower(c); });
        animals[i] = word;
        i++;
    }
    f_animals.close();
    sizeAnimals = i;
}

//usuwa klienta
void removeClient(int fd)
{
    for(int i = 0; i < numClients; i++)
    {
        if (clients[i].fd == fd)
        {
            for(int j = i; j < numClients - 1; j++)
            {
                clients[j] = clients[j + 1];
            }
        }
    }
    numClients--;
}
//dodaje klienta
void addClient(int fd,string name)
{
    clients[numClients].fd = fd;
    clients[numClients].name = name;
    clients[numClients].points = 0;
    numClients++;
}

//losuje litery w zaleznosci od ilosci rund
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
//odlicza czas rundy
int countdown(int t)
{
    for (int i = t; i > 0; i--) {
        
        if(numClients < 2) return i;
        if(roundAnswers == roundClients) return 0;
        if(stopper == 1) 
            {
                if(i > time_after_first)
                {
                    stopper = 0;
                    return 0;
                }

            }
        std::cout << std::endl<<i;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
//dodaje na poczatku wiadomosci jego typ i wielkosc
string filler(int x,string type)
{
    string fill = to_string(x);
    for(int i = fill.size(); i < 4 ;i++)
        fill.insert(0,"0");
    fill.insert(0,type);
    return fill;
}


//wysyla wszystkim klientom
void sendToAll(char * buffer, int count){
    int res;
    std::unique_lock<std::mutex> lock(clientFdsLock);
    //cout<<endl<<"buffer----"<<buffer<<"----count "<<count<<endl;
    for(int i = 0;i < numClients; i++){
        res = send(clients[i].fd, buffer, count, MSG_DONTWAIT);
    }
}
//wysyla imiona i punkty wszystkich klientow
void sendNames(int fd)
{
    char buffer[255];
    int res;
    int sizer;
    string message;
    string helper;
    for(int i = 0; i < numClients; i++)
        message.append(clients[i].name + "-" + to_string(clients[i].points) + ",");
    sizer = message.size();
    message.insert(0,filler(sizer,"1"));    
    
    strcpy(buffer, message.c_str());

    //std::unique_lock<std::mutex> lock(clientFdsLock);
    res = send(fd, buffer, strlen(buffer), MSG_DONTWAIT);
}
//przypisuje do klienta jego propozycje hasel
void guesses_to_player(int fd, char * guesses)
{
    string text = "";
    int counter = 0;
    roundAnswers++;
    for(int i = 0; i < numClients; i++)
    {
        if(clients[i].fd == fd)
        {
            for(int x = 0; x < strlen(guesses); x++)
            {
                if(guesses[x] == ',')
                {
                    clients[i].given[counter] = text;
                    text.clear();
                    counter++;
                }
                else
                    text = text + guesses[x];
            }
            cout<<" 1 "<<clients[i].given[0]<<" 2 "<<clients[i].given[1]<<" 3 "<<clients[i].given[2]<<" 4 "<<clients[i].given[3]<<" fd - "<<clients[i].fd<<endl;
            break;
        }


    }


}
//daje punkty po rundzie

void givePoints()
{/*
    for(int i = 0; i < numClients)
    {
        for(int a = 0 ; a < sizeCountries; a++)
        {


        }
    }
    
*/
printf("essa");
}

//1 - imiona z punktacja 2 - literka z czasem 3 - pozostały czas
//panstwo,miasta,roslina,zwierze,
//prowadzi gre
void game(){
    char round_letters[rounds]; 
    string message;
    string helper;
    int sizer;
    char buffer[255];
    helper = "";
    message.append(helper);
    int err;
    while(true)
    {
        if(numClients > 1)
            {
                countdown(5);
                roundClients = numClients;

                random_letters(round_letters);
                cout << round_letters;
                helper = round_letters[0];

               // for(int i = 0; i < numClients; i++)
                   // sendNames(clients[i].fd);

                for(int i = 0; i < numClients; i++)
                    sendNames(clients[i].fd);
                countdown(3);
                message.append(helper + "," + to_string(time_for_round));
                sizer = message.size();
                message.insert(0,filler(sizer,"2"));
                strcpy(buffer, message.c_str());
                sendToAll(buffer,sizer + 5);
                
                
                err = countdown(time_for_round);
                if(err == 0)
                {
                    sendToAll(buffer,sizer + 5);

                message.clear();
                message.append(helper + "," + to_string(time_after_first));
                sizer = message.size();
                message.insert(0,filler(sizer,"2"));
                strcpy(buffer, message.c_str());
                sendToAll(buffer,sizer + 5);
                countdown(time_after_first);

                givePoints();

                roundAnswers = 0;

                }
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

    getData();

    /*
    for(int i=0;i<sizeCountries;i++)
    cout<<countries[i]<<" ";
    for(int i=0;i<sizeAnimals;i++)
    cout<<animals[i]<<" ";
    for(int i=0;i<sizePolNames;i++)
    cout<<polNames[i]<<" ";
    for(int i=0;i<sizeCities;i++)
    cout<<cities[i]<<" ";*/

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
                addClient(clientFd,"Mariola");
                }


                printf("new connection from: %s:%hu (fd: %d)\n ", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port), clientFd);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP)){
                printf("[+] connection closed\n");
                epoll_ctl(myepoll, EPOLL_CTL_DEL,events[i].data.fd, NULL);
                close(events[i].data.fd);
                removeClient(events[i].data.fd);
            continue;
            }
            else if(events[i].events & EPOLLIN)
            {
                char buf[512];
                int n = read(events[i].data.fd, buf, 512);
                stopper = 1;
                cout<<"[+] data: ";
                guesses_to_player(events[i].data.fd,buf);
            }


        }

        }


	return 0;
}


