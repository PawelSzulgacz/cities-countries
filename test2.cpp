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
#include <pthread.h>

using namespace std;
//liczba rund, czas na runde, czas na runde gdy 1 wysle hasla,litery do losowania, maksymalna liczba klientow
const size_t rounds = 2;
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
    string name = "";
    int points = 0;
    string given[4];
};
//tablica klientow, liczba klientow w tym momencie, zmienna inicjalizujaca stop gry, gdy jest za malo klientow, klienci w danej rundzie, odpowiedzi klientow w danej rundzie
Client clients[1000];
int numClients = 0;
int numClientsLogged = 0;
int stopper = 0;
int roundClients = 2;
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
            clients[i].name = "";
            clients[i].points = 0;
            for(int j = i; j < numClients; j++)
            {
                clients[j] = clients[j + 1];
            }
        }
    }
    numClients--;
}
//dodaje klienta
void addClient(int fd)
{
    clients[numClients].fd = fd;
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
        
        if(numClientsLogged < 2) return -1;
        if(roundAnswers == roundClients) return 0;
        if(stopper == 1) 
            {
                stopper = 0;
                if(i > time_after_first)
                {
                    return time_after_first;
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
   // for(int i = fill.size(); i < 4 ;i++)
       // fill.insert(0,"0");
    fill.insert(0,type);
    return fill;
}

int find_numer_of_client(int fd)
{
    for(int b = 0; b < numClients; b++)
    {
        if(clients[b].fd == fd)
        return b;
    }
    return 0;
}


//wysyla wszystkim klientom
void sendToAll(char * buffer, int count){
    int res;
    std::unique_lock<std::mutex> lock(clientFdsLock);
    //cout<<endl<<"buffer----"<<buffer<<"----count "<<count<<endl;
    for(int i = 0;i < numClients; i++){
        if(clients[i].name != "")
            res = send(clients[i].fd, buffer, count, MSG_DONTWAIT);
    }
}
//wysyla imiona i punkty wszystkich klientow
void sendNames(int fd,string type)
{
    char buffer[255];
    int res;
    int sizer;
    string message;
    string helper;
    for(int i = 0; i < numClients; i++)
        message.append(clients[i].name + "-" + to_string(clients[i].points) + ",");
    sizer = message.size();
    //message.insert(0,filler(sizer,"1"));    
    message.insert(0,type);  
    message.append("#");  
    strcpy(buffer, message.c_str());

    //std::unique_lock<std::mutex> lock(clientFdsLock);
    res = send(fd, buffer, strlen(buffer), MSG_DONTWAIT);
}
//przypisuje do klienta jego propozycje hasel COS NIE TAK
void guesses_to_player(int number, char * guesses,int n)
{
    string text = "";
    int counter = 0;
    roundAnswers++;

    for(int x = 0; x < n; x++)
    {
        if(guesses[x] == ',')
        {
            clients[number].given[counter] = text;
            text.clear();
            counter++;
        }
        else
            text = text + guesses[x];
    }
    cout<<clients[number].name<<" fd - "<<clients[number].fd<<" 1 "<<clients[number].given[0]<<" 2 "<<clients[number].given[1]<<" 3 "<<clients[number].given[2]<<" 4 "<<clients[number].given[3]<<endl;
    text.clear();



}
//daje punkty po rundzie
//

void add_point(string * data,int size,int fd,char letter,int i,int which)
{
    string word;
    string oppword;
    int apoint = 0;

    word = clients[i].given[which];
    std::transform(word.begin(), word.end(), word.begin(),[](unsigned char c){ return std::tolower(c); });
    if(word[0] == letter)
    {
        for(int a = 0 ; a < size; a++)
        {
            if(word == data[a])
            {
                for(int c = 0; c < numClients; c++)
                {
                    oppword = clients[c].given[which];
                    std::transform(oppword.begin(), oppword.end(), oppword.begin(),[](unsigned char d){ return std::tolower(d); });
                    if(oppword == word && clients[c].fd != clients[i].fd)
                        apoint = 5;
                }
                if(apoint != 5)
                    apoint = 10;
                clients[i].points += apoint;
                break;
            }
        }
    }

}




void givePoints(char letter)
{

    for(int i = 0; i < numClients; i++)
    {

        add_point(countries,sizeCountries,clients[i].fd,letter,i,0);
        add_point(cities,sizeCities,clients[i].fd,letter,i,1);
        add_point(polNames,sizePolNames,clients[i].fd,letter,i,2);
        add_point(animals,sizeAnimals,clients[i].fd,letter,i,3);
        cout<<endl<<clients[i].name<<" punkty - "<<clients[i].points<<" fd - "<<clients[i].fd;
    }
    
}

void winner()
{
    int max = 0;
    int winner = 0;
    for(int i = 0; i < numClients; i++)
    {
        if(clients[i].points > max)
        {
            max = clients[i].points; 
            winner = i;
        }
    }
    cout<<"Winner - "<<clients[winner].name<<" with "<<clients[winner].points<<" points"<<endl;
}
void deleting_points()
{
        for(int i = 0; i < numClients; i++)
    {
        clients[i].points = 0;
    }
}



void send_letter_time(string letter, int t)
{
    char buffer[255];
    string mess = "";
    int sizer;
    mess.append(letter + "," + to_string(t));
    mess.insert(0,"2");
    mess.append("#");
    strcpy(buffer, mess.c_str());
    sizer = mess.size();
    sendToAll(buffer,sizer);
}
//jak sie jeden rozlaczy to gra sie nie startuje znowu
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
        if(numClientsLogged > 1)
            {
                for(int i=0;i < rounds; i++)
                {
                    countdown(5);
                    if(numClientsLogged < 2 ) break;
                    roundClients = numClientsLogged;

                    random_letters(round_letters);
                    cout << round_letters[i];
                    helper = round_letters[i];

                // for(int i = 0; i < numClients; i++)
                    // sendNames(clients[i].fd);

                    for(int i = 0; i < numClients; i++)
                        sendNames(clients[i].fd,"1");



                    send_letter_time(helper,time_for_round);
                    
                    
                    err = countdown(time_for_round);
                    if(err > 0)
                    {
                        if(err == time_after_first)
                        {
                            send_letter_time(helper,time_after_first);
                            countdown(time_after_first);
                        }


                        givePoints(round_letters[i]);

                        roundAnswers = 0;
                        roundClients = 2;

                    }
                    else 
                    {
                        break;
                    }

                    stopper = 0;
                    }
                    for(int i = 0; i < numClients; i++)
                    sendNames(clients[i].fd,"3");

                    //winner();
                    deleting_points();
                    countdown(30);
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
                addClient(clientFd);
                }


                printf("new connection from: %s:%hu (fd: %d)\n ", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port), clientFd);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP)){
                printf("[+] connection closed\n");
                epoll_ctl(myepoll, EPOLL_CTL_DEL,events[i].data.fd, NULL);
                close(events[i].data.fd);
                removeClient(events[i].data.fd);
                numClientsLogged--;
            continue;
            }
            else if(events[i].events & EPOLLIN)
            {
                char buf[512];
                int number = find_numer_of_client(events[i].data.fd);
                int n = read(events[i].data.fd, buf, 512);
                if(clients[number].name == "")
                {
                    string name;
                    for(int a = 0; a < n; a++)
                    name += buf[a];
                    
                    clients[number].name = name;
                    numClientsLogged++;
                    cout<<"[+] data: "<<name;
                }
                else
                {
                    cout<<"[+] data: ";
                    guesses_to_player(number,buf,n);
                    stopper = 1;
                }


            }


        }

        }


	return 0;
}


