#define BUFSIZE 100
#define WORD_LEN 5
#define NUM_GUESSES 6
#define MAX_THREADS 130

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>


extern int total_guesses;
extern int total_wins;
extern int total_losses;
extern char ** words;

int port;
int seed;
FILE *fp;
char* filename;
int num_words;
char** wordle_words;
int num_words_played;
int listener;
int run_loop;
intptr_t* newsd_array;
int total_num_threads;

pthread_mutex_t mtx=PTHREAD_MUTEX_INITIALIZER;

struct thread_args{
    int newsd;
    pthread_t thread_id;
};

void signalHandler(int sig) {
    if (sig == SIGUSR1){
        printf("MAIN: SIGUSR1 rcvd; Wordle server shutting down...\n");
        printf("MAIN: Wordle server shutting down...\n");
        close(listener);
        run_loop = 0;
    }
}

int check_valid_word(char* guess){
    for(int i=0;i!=num_words;i++){
        if(strcasecmp (guess, *(wordle_words+i)) == 0){
            return 1;
        }
    }
    return 0;
}

int letter_exists(char letter, char*answer){
    for(int i=0; i!= WORD_LEN; i++){
        if(tolower(letter) == tolower(*(answer+i))){
            return 1;
        }
    }
    return 0;
}


void create_reply(char* guess, char*answer, char*reply){
    char letter;
    for(int i=0; i!= WORD_LEN; i++){
        letter = toupper(*(guess+i));
        if(letter == toupper(*(answer+i)) ){
            *(reply+i) = toupper(letter);
        }else if(letter_exists(letter, answer)){
            *(reply+i) = tolower(letter);
        }else{
            *(reply+i) = '-';
        }
    }
}

void* handle_connection(void * args){
    //init random word to guess
    int word_index = rand() % num_words;
    char* my_word = *(wordle_words + word_index);
    //upper case my word
    for(int i=0;i!= WORD_LEN;i++) {
        *(my_word+i) = toupper(*(my_word+i));
    }
    pthread_mutex_lock(&mtx);
    words = realloc(words, (num_words_played+2) * sizeof(char*));
    *(words+num_words_played) = calloc(WORD_LEN+1, sizeof(char));
    strcpy(*(words+num_words_played), my_word);
    *(words+num_words_played+1) = NULL;
    num_words_played ++;
    pthread_mutex_unlock(&mtx);


    intptr_t newsd = ((struct thread_args*) args)->newsd;
    pthread_t thread_id = ((struct thread_args*) args)->thread_id;
    pthread_mutex_lock(&mtx);
    *(newsd_array+total_num_threads) = newsd;
    total_num_threads++;
    pthread_mutex_unlock(&mtx);
    char* buffer = calloc(BUFSIZE+1, sizeof(char));
    char* guess = calloc(5+1, sizeof(char));
    char* message = (char*)calloc(9, sizeof(char));
    char * reply = (char*)calloc(WORD_LEN+1, sizeof(char));



    short remaining_guesses = NUM_GUESSES;
    while(remaining_guesses){
        printf("THREAD %lu: waiting for guess\n", thread_id);
        fflush(stdout);
        //read client mssg
        int n = recv(newsd, buffer, BUFSIZE, 0);
        if(n==0){
            printf("THREAD %lu: client gave up; closing TCP connection...\n", thread_id);
            pthread_mutex_lock(&mtx);
            total_losses++;
            pthread_mutex_unlock(&mtx);
            break;
        }
        if(n==-1){
            perror("read() failed");
        }

        memcpy(guess, buffer, 5 );
        *(guess + 6) = '\0';
        printf("THREAD %lu: rcvd guess: %s\n", thread_id, guess);
        fflush(stdout);
        //send client mssg
        if(check_valid_word(guess)){
            remaining_guesses --;
            pthread_mutex_lock(&mtx);
            total_guesses++;
            pthread_mutex_unlock(&mtx);

            *(message) = 'Y';
            *(short *) (message + 1) = htons(remaining_guesses);
            //create a reply
            create_reply(guess, my_word, reply);
            int j=0;
            for(int i=3; i!=8; i++){
                *(message+i) = *(reply+j);
                j++;
            }
            // *(message+9) = '\0';
            if(remaining_guesses==1){
                printf("THREAD %lu: sending reply: %s (%d guess left)\n", thread_id, reply, remaining_guesses);
            }else{
                printf("THREAD %lu: sending reply: %s (%d guesses left)\n", thread_id, reply, remaining_guesses);
            }
        }else{
            *(message) = 'N';
            if(remaining_guesses==1){
                printf("THREAD %lu: invalid guess; sending reply: ????? (%d guess left)\n", thread_id, remaining_guesses);
            }else{
                printf("THREAD %lu: invalid guess; sending reply: ????? (%d guesses left)\n", thread_id, remaining_guesses);
            }
            fflush(stdout);
        }
        write(newsd, message, 9);
        fflush(stdout);
        //win game!
        if(strcasecmp (guess, my_word) ==0){
            fflush(stdout);
            pthread_mutex_lock(&mtx);
            total_wins++;
            pthread_mutex_unlock(&mtx);
            break;
        }
    }

    printf("THREAD %lu: game over; word was %s!\n", thread_id, my_word);
    fflush(stdout);
    pthread_mutex_lock(&mtx);
    if(remaining_guesses==0){
        total_losses++;
    }
    pthread_mutex_unlock(&mtx);

    free(message);
    free(reply);
    free(args);
    free(buffer);
    free(guess);

    
    return EXIT_SUCCESS;
}

int wordle_server(int argc, char ** argv ){
    signal(SIGUSR1, signalHandler);

    port = atoi(*(argv + 1));
    seed = atoi(*(argv + 2)); 
    filename = *(argv + 3);
    fp = fopen(filename, "r" );
    num_words = atoi(*(argv + 4)); 

    if(port < 0 || num_words < 0 || argc < 5){
        printf("ERROR: Invalid argument(s)");
        return EXIT_FAILURE;
    }

    srand(seed);
    //init words_played,total_num_threads
    num_words_played = total_num_threads = 0;

    newsd_array = (intptr_t*)calloc(MAX_THREADS, sizeof(intptr_t));

    //parse words from wordle-words.txt
    wordle_words = (char**)calloc(num_words, sizeof(char*));
    for(int i=0;i<num_words;++i){
        *(wordle_words + i) = calloc(WORD_LEN + 1, sizeof(char));
        fscanf(fp, "%s", *(wordle_words+i));
    }

    pthread_t thread_id = 0;
    printf("MAIN: opened %s (%d words)\n", filename, num_words);
    
    listener = socket( AF_INET, SOCK_STREAM, 0 );
    if ( listener == -1 ) { perror( "socket() failed" ); return EXIT_FAILURE; }

    //populate socket for binding
    struct sockaddr_in tcp_server;
    tcp_server.sin_family = AF_INET;
    tcp_server.sin_port = htons(port); 
    tcp_server.sin_addr.s_addr = htonl(INADDR_ANY); ///allow any ip to connect

    if(bind(listener, (struct sockaddr *)&tcp_server, sizeof(tcp_server)) == -1){
        perror("bind() failed");
        return EXIT_FAILURE;
    }

    if(listen(listener, WORD_LEN)== -1){
        perror("listen() failed");
        return EXIT_FAILURE;
    }

    printf("MAIN: Wordle server listening on port {%d}\n", port);

    //loop to handle connections
    run_loop = 1;
    while(run_loop){
        struct sockaddr_in remote_client;
        int addrlen = sizeof(remote_client);
        fflush(stdout);
          
        intptr_t newsd = accept(listener, (struct sockaddr *)&remote_client, (socklen_t *)&addrlen);
        if(run_loop==0){
            break;
        }
        printf("MAIN: rcvd incoming connection request\n");
        if(newsd==-1){
            perror("accept() failed");
            return EXIT_FAILURE;
        }
        struct thread_args *args = (struct thread_args *)calloc(2, sizeof(struct thread_args));
        args->newsd = newsd;
        args->thread_id=thread_id;
        pthread_create(&thread_id, NULL, handle_connection, (void*) args);
        pthread_detach(thread_id);
    }   
    /* deallocate memory for the list of wordle words */
    for(int i =0;i!=num_words;i++){
        free( *(wordle_words+i) );
    }
    free(wordle_words);
    fclose(fp);
    for(int i=0;i!=total_num_threads;i++){
        close(*(newsd_array+i));
    }
    free(newsd_array);
    return EXIT_SUCCESS;
}