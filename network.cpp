#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

extern volatile bool interrupt_received;

void listenForData(int port) 
{
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    char buffer[1024];
    socklen_t len;
    int n;

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Socket creation failed." << std::endl;
        exit(EXIT_FAILURE);
    }
    
    // Zero out the memory for the server address
    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    // Set up server address structure
    servaddr.sin_family = AF_INET;          // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;  // Listen on all available interfaces
    servaddr.sin_port = htons(port);        // Port number

    // Bind the socket to the port
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) 
    {
        std::cerr << "Bind failed." << std::endl;
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Listening for data on port " << port << "..." << std::endl;

    // Loop to listen for incoming data
    while (!interrupt_received) 
    {
        len = sizeof(cliaddr);
        n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&cliaddr, &len);
        if (n < 0) 
        {
            std::cerr << "Error receiving data." << std::endl;
        } 
        else 
        {
            buffer[n] = '\0'; // Null-terminate the received data
            std::cout << "Received: " << buffer << std::endl;
        }
    }

    // Close the socket (unreachable in this infinite loop, but good practice)
    close(sockfd);
}
