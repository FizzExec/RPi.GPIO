/*
Copyright (c) 2013-2018 Ben Croston

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <pthread.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include "event_gpio.h"

extern int bpi_found;
extern int bpi_found_mtk;
extern int bpi_debug;
extern int *pinTobcm_BP;

#define GPIO_ALL -666
#define NO_BOUNCETIME -666

const char *stredge[4] = {"none", "rising", "falling", "both"};

struct gpios *gpio_list = NULL;

// event callbacks
struct callback
{
    unsigned int gpio;
    void (*func)(unsigned int gpio);
    struct callback *next;
};
struct callback *callbacks = NULL;

pthread_t threads;
int event_occurred[54] = { 0 };
int thread_running = 0;
int epfd_thread = -1;
int epfd_blocking = -1;

/************* /sys/class/gpio functions ************/
#define x_write(fd, buf, len) do {                                  \
    size_t x_write_len = (len);                                     \
                                                                    \
    if ((size_t)write((fd), (buf), x_write_len) != x_write_len) {   \
        close(fd);                                                  \
        return (-1);                                                \
    }                                                               \
} while (/* CONSTCOND */ 0)

int gpio_export(unsigned int gpio)
{
    int fd, len;
    char str_gpio[4];
    char filename[33];

    if (1==bpi_found) {
      gpio = *(pinTobcm_BP + gpio);
      if (bpi_debug>=4) printf("translated gpio=%u\n", gpio);
    }
    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d", gpio);
    if (bpi_debug>=4) printf("access %s\n", filename);

    /* return if gpio already exported */
    if (access(filename, F_OK) != -1) {
      if (bpi_debug>=4) printf("access file '%s' already done\n", filename);
      return 0;
    }

    if ((fd = open("/sys/class/gpio/export", O_WRONLY)) < 0) {
      if (bpi_debug>=1) printf("open gpio export file failed\n");
      return -1;
    }

    //if (bpi_debug>=4) printf("export %u\n", gpio);
    len = snprintf(str_gpio, sizeof(str_gpio), "%d", gpio);
    x_write(fd, str_gpio, len);
    close(fd);

    return 0;
}

int gpio_unexport(unsigned int gpio)
{
    int fd, len;
    char str_gpio[4];

    if (1==bpi_found) {
      gpio = *(pinTobcm_BP + gpio);
      if (bpi_debug>=4) printf("translated gpio=%u\n", gpio);
    }   
    if ((fd = open("/sys/class/gpio/unexport", O_WRONLY)) < 0) {
        if (bpi_debug>=1) printf("open gpio unexport file failed\n");
        return -1;
    }
    len = snprintf(str_gpio, sizeof(str_gpio), "%d", gpio);
    x_write(fd, str_gpio, len);
    close(fd);

    return 0;
}

int gpio_set_direction(unsigned int gpio, unsigned int in_flag)
{
    int retry;
    struct timespec delay;
    int fd;
    char filename[34];

    if (1==bpi_found) {
      gpio = *(pinTobcm_BP + gpio);
      if (bpi_debug>=4) printf("translated gpio=%u\n", gpio);
    }
    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/direction", gpio);

    // retry waiting for udev to set correct file permissions
    delay.tv_sec = 0;
    delay.tv_nsec = 10000000L; // 10ms
    for (retry=0; retry<100; retry++) {
        if ((fd = open(filename, O_WRONLY)) >= 0)
            break;
        nanosleep(&delay, NULL);
    }
    if (retry >= 100) {
      if (bpi_debug>=1) printf("open gpio direction file '%s' failed\n", filename);
      return -1;
    }

    if (in_flag)
        x_write(fd, "in", 3);
    else
        x_write(fd, "out", 4);

    close(fd);
    return 0;
}

int gpio_set_edge(unsigned int gpio, unsigned int edge)
{
    int fd;
    char filename[29];
    if (bpi_debug>=4) printf("gpio_set_edge gpio=%u, edge=%u\n",gpio, edge);

    if (1==bpi_found) {
      gpio = *(pinTobcm_BP + gpio);
      if (bpi_debug>=4) printf("translated gpio=%u\n", gpio);
    }
    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/edge", gpio);

    if ((fd = open(filename, O_WRONLY)) < 0) {
        if (bpi_debug>=1) printf("open gpio edge file '%s' failed\n", filename);
        return -1;
    }
    if (bpi_debug>=4) printf("edge=%s\n", stredge[edge]);
    x_write(fd, stredge[edge], strlen(stredge[edge]) + 1);
    close(fd);
    return 0;
}

int open_value_file(unsigned int gpio)
{
    int fd;
    char filename[30];

    if (bpi_debug>=4) printf("open_value_file gpio=%u\n",gpio);
    if (1==bpi_found) {
      gpio = *(pinTobcm_BP + gpio);
      if (bpi_debug>=4) printf("translated gpio=%u\n", gpio);
    }
    // create file descriptor of value file
    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/value", gpio);
    if ((fd = open(filename, O_RDONLY | O_NONBLOCK)) < 0) {
        if (bpi_debug>=1) printf("open gpio value file '%s' failed\n", filename);
        return -1;
    }
    return fd;
}

/********* gpio list functions **********/
struct gpios *get_gpio(unsigned int gpio)
{
    struct gpios *g = gpio_list;
    while (g != NULL) {
        if (g->gpio == gpio)
            return g;
        g = g->next;
    }
    return NULL;
}

struct gpios *get_gpio_from_value_fd(int fd)
{
    struct gpios *g = gpio_list;
    while (g != NULL) {
        if (g->value_fd == fd)
           return g;
        g = g->next;
    }
    return NULL;
}

struct gpios *new_gpio(unsigned int gpio)
{
    struct gpios *new_gpio;

    if (bpi_debug>=4) printf("new_gpio gpio=%u\n", gpio);
    new_gpio = malloc(sizeof(struct gpios));
    if (new_gpio == 0) {
      if (bpi_debug>=1) printf("new_gpio memory error\n");
      return NULL;  // out of memory
    }

    new_gpio->gpio = gpio;
    if (gpio_export(gpio) != 0) {
        free(new_gpio);
        if (bpi_debug>=1) printf("new_gpio gpio_export failed\n");
        return NULL;
    }
    new_gpio->exported = 1;

    if (1!=bpi_found) {
      if (gpio_set_direction(gpio,1) != 0) { // 1==input
          free(new_gpio);
          if (bpi_debug>=1) printf("new_gpio gpio_set_direction failed\n");
          return NULL;
      }
    }

    if ((new_gpio->value_fd = open_value_file(gpio)) == -1) {
        gpio_unexport(gpio);
        free(new_gpio);
        if (bpi_debug>=1) printf("new_gpio open_value_file failed\n");
        return NULL;
    }

    new_gpio->initial_thread = 1;
    new_gpio->initial_wait = 1;
    new_gpio->bouncetime = NO_BOUNCETIME;
    new_gpio->lastcall = 0;
    new_gpio->thread_added = 0;

    if (gpio_list == NULL) {
        new_gpio->next = NULL;
    } else {
        new_gpio->next = gpio_list;
    }
    gpio_list = new_gpio;
    if (bpi_debug>=4) printf("new_gpio succeeded\n");
    return new_gpio;
}

void delete_gpio(unsigned int gpio)
{
    struct gpios *g = gpio_list;
    struct gpios *prev = NULL;

    while (g != NULL) {
        if (g->gpio == gpio) {
            if (prev == NULL)
                gpio_list = g->next;
            else
                prev->next = g->next;
            free(g);
            return;
        } else {
            prev = g;
            g = g->next;
        }
    }
}

int gpio_event_added(unsigned int gpio)
{
    struct gpios *g = gpio_list;
    while (g != NULL) {
        if (g->gpio == gpio)
            return g->edge;
        g = g->next;
    }
    return 0;
}

/******* callback list functions ********/
int add_edge_callback(unsigned int gpio, void (*func)(unsigned int gpio))
{
    struct callback *cb = callbacks;
    struct callback *new_cb;

    new_cb = malloc(sizeof(struct callback));
    if (new_cb == 0)
        return -1;  // out of memory

    new_cb->gpio = gpio;
    new_cb->func = func;
    new_cb->next = NULL;

    if (callbacks == NULL) {
        // start new list
        callbacks = new_cb;
    } else {
        // add to end of list
        while (cb->next != NULL)
            cb = cb->next;
        cb->next = new_cb;
    }
    return 0;
}

int callback_exists(unsigned int gpio)
{
    struct callback *cb = callbacks;

    while (cb != NULL) {
        if (cb->gpio == gpio)
            return 1;
        cb = cb->next;
    }
    return 0;
}

void run_callbacks(unsigned int gpio)
{
    struct callback *cb = callbacks;
    while (cb != NULL)
    {
        if (cb->gpio == gpio)
            cb->func(cb->gpio);
        cb = cb->next;
    }
}

void remove_callbacks(unsigned int gpio)
{
    struct callback *cb = callbacks;
    struct callback *temp;
    struct callback *prev = NULL;

    while (cb != NULL)
    {
        if (cb->gpio == gpio)
        {
            if (prev == NULL)
                callbacks = cb->next;
            else
                prev->next = cb->next;
            temp = cb;
            cb = cb->next;
            free(temp);
        } else {
            prev = cb;
            cb = cb->next;
        }
    }
}

void *poll_thread(void *threadarg)
{
    struct epoll_event events;
    char buf;
    struct timeval tv_timenow;
    unsigned long long timenow;
    struct gpios *g;
    int n;
    if (bpi_debug>=4) printf("poll_thread\n");

    thread_running = 1;
    while (thread_running) {
        if (bpi_debug>=4) printf("poll_thread epoll_wait\n");
        n = epoll_wait(epfd_thread, &events, 1, -1);
        if (n > 0) {
            if (bpi_debug>=4) printf("poll_thread data\n");
            lseek(events.data.fd, 0, SEEK_SET);
            if (read(events.data.fd, &buf, 1) != 1) {
                thread_running = 0;
                if (bpi_debug>=1) printf("poll_thread exit read\n");
                pthread_exit(NULL);
            }
            if (bpi_debug>=4) printf("poll_thread get_gpio_from_value_fd\n");
            g = get_gpio_from_value_fd(events.data.fd);
            if (g->initial_thread) {     // ignore first epoll trigger
                g->initial_thread = 0;
            } else {
                gettimeofday(&tv_timenow, NULL);
                timenow = tv_timenow.tv_sec*1E6 + tv_timenow.tv_usec;
                if (NO_BOUNCETIME==g->bouncetime || timenow - g->lastcall > (unsigned int)g->bouncetime*1000 || g->lastcall == 0 || g->lastcall > timenow) {
                    if (bpi_debug>=4) printf("poll_thread EVENT\n");
                    g->lastcall = timenow;
                    event_occurred[g->gpio] = 1;
                    run_callbacks(g->gpio);
                }
            }
        } else if (n == -1) {
            /*  If a signal is received while we are waiting,
                epoll_wait will return with an EINTR error.
                Just try again in that case.  */
            if (errno == EINTR) {
                continue;
            }
            thread_running = 0;
            pthread_exit(NULL);
            if (bpi_debug>=1) printf("poll_thread exit wait -1\n");
        }
    }
    thread_running = 0;
    pthread_exit(NULL);
    if (bpi_debug>=4) printf("poll_thread exit\n");
}


void remove_edge_detect(unsigned int gpio)
{
    if (bpi_debug>=4) printf("remove_edge_detect gpio=%u\n",gpio);

    struct epoll_event ev;
    struct gpios *g = get_gpio(gpio);

    if (g == NULL)
        return;

    // delete epoll of fd

    ev.events = EPOLLIN | EPOLLET | EPOLLPRI;
    ev.data.fd = g->value_fd;
    epoll_ctl(epfd_thread, EPOLL_CTL_DEL, g->value_fd, &ev);

    // delete callbacks for gpio
    remove_callbacks(gpio);

    // btc fixme - check return result??
    gpio_set_edge(gpio, NO_EDGE);
    g->edge = NO_EDGE;

    if (g->value_fd != -1)
        close(g->value_fd);

    // btc fixme - check return result??
    gpio_unexport(gpio);
    event_occurred[gpio] = 0;

    delete_gpio(gpio);
}

int event_detected(unsigned int gpio)
{
    if (bpi_debug>=4) printf("event_detected gpio=%u\n",gpio);
    if (event_occurred[gpio]) {
        event_occurred[gpio] = 0;
        return 1;
    } else {
        return 0;
    }
}

void event_cleanup(int gpio)
// gpio of GPIO_ALL means clean every channel used
{
    struct gpios *g = gpio_list;
    struct gpios *next_gpio = NULL;

    if (bpi_debug>=4) {
      if (GPIO_ALL==gpio) {
        printf("event_cleanup all gpios\n");
      } else {
       printf("event_cleanup gpio=%d\n",gpio);
      }
    }
    while (g != NULL) {
        next_gpio = g->next;
        if ((GPIO_ALL == gpio) || ((int)g->gpio == gpio))
            remove_edge_detect(g->gpio);
        g = next_gpio;
    }
    if (gpio_list == NULL) {
        if (epfd_blocking != -1) {
            close(epfd_blocking);
            epfd_blocking = -1;
        }
        if (epfd_thread != -1) {
            close(epfd_thread);
            epfd_thread = -1;
        }
        thread_running = 0;
    }
}

void event_cleanup_all(void)
{
   event_cleanup(GPIO_ALL);
}

int add_edge_detect(unsigned int gpio, unsigned int edge, int bouncetime)
// return values:
// 0 - Success
// 1 - Edge detection already added
// 2 - Other error
{
    pthread_t threads;
    struct epoll_event ev;
    long t = 0;
    struct gpios *g;
    int i = -1;

    if (bpi_debug>=4) printf("add_edge_detect gpio=%u, edge=%u, bouncetime=%d\n",gpio, edge, bouncetime);
    i = gpio_event_added(gpio);
    if (i == 0) {    // event not already added
        if ((g = new_gpio(gpio)) == NULL) {
            if (bpi_debug>=1) printf("error 2 (event not created)\n");
            return 2;
        }
        gpio_set_edge(gpio, edge);
        g->edge = edge;
        g->bouncetime = bouncetime;
    } else if (i == (int)edge) {  // get existing event
        g = get_gpio(gpio);
        if ((bouncetime != NO_BOUNCETIME && g->bouncetime != bouncetime) ||  // different event bouncetime used
            (g->thread_added))  {               // event already added
            if (bpi_debug>=1) printf("error 1 (event already added)\n");
            return 1;
        }
    } else {
        if (bpi_debug>=1) printf("error 1 (wrong event state)\n");
        return 1;
    }

    // create epfd_thread if not already open
    if ((epfd_thread == -1) && ((epfd_thread = epoll_create(1)) == -1)) {
        if (bpi_debug>=1) printf("error 2 (epoll creation failed)\n");
        return 2;
    }

    // add to epoll fd
    ev.events = EPOLLIN | EPOLLET | EPOLLPRI;
    ev.data.fd = g->value_fd;
    if (epoll_ctl(epfd_thread, EPOLL_CTL_ADD, g->value_fd, &ev) == -1) {
        if (bpi_debug>=1) printf("error 2 (epoll_ctl failed)\n");
        remove_edge_detect(gpio);
        return 2;
    }
    g->thread_added = 1;

    // start poll thread if it is not already running
    if (!thread_running) {
        if (pthread_create(&threads, NULL, poll_thread, (void *)t) != 0) {
           if (bpi_debug>=1) printf("error 2 (event thread creation failed)\n");
           remove_edge_detect(gpio);
           return 2;
        }
    }
    if (bpi_debug>=4) printf("add_edge_detect succeed\n");
    return 0;
}

int blocking_wait_for_edge(unsigned int gpio, unsigned int edge, int bouncetime, int timeout)
// return values:
//    1 - Success (edge detected)
//    0 - Timeout
//   -1 - Edge detection already added
//   -2 - Other error
{
    int n, ed;
    struct epoll_event events, ev;
    char buf;
    struct gpios *g = NULL;
    struct timeval tv_timenow;
    unsigned long long timenow;
    int finished = 0;
    int initial_edge = 1;

    if (bpi_debug>=4) printf("blocking_wait_for_edge gpio=%u, edge=%u, bouncetime=%d, timeout=%d\n",gpio, edge, bouncetime, timeout);
    if (callback_exists(gpio))
        return -1;

    // add gpio if it has not been added already
    ed = gpio_event_added(gpio);
    if (ed == (int)edge) {   // get existing record
        g = get_gpio(gpio);
        if (g->bouncetime != NO_BOUNCETIME && g->bouncetime != bouncetime) {
            return -1;
        }
    } else if (ed == NO_EDGE) {   // not found so add event
        if ((g = new_gpio(gpio)) == NULL) {
            return -2;
        }
        gpio_set_edge(gpio, edge);
        g->edge = edge;
        g->bouncetime = bouncetime;
    } else {    // ed != edge - event for a different edge
        g = get_gpio(gpio);
        gpio_set_edge(gpio, edge);
        g->edge = edge;
        g->bouncetime = bouncetime;
        g->initial_wait = 1;
    }

    // create epfd_blocking if not already open
    if ((epfd_blocking == -1) && ((epfd_blocking = epoll_create(1)) == -1)) {
        return -2;
    }

    // add to epoll fd
    ev.events = EPOLLIN | EPOLLET | EPOLLPRI;
    ev.data.fd = g->value_fd;
    if (epoll_ctl(epfd_blocking, EPOLL_CTL_ADD, g->value_fd, &ev) == -1) {
        return -2;
    }

    // wait for edge
    while (!finished) {
        n = epoll_wait(epfd_blocking, &events, 1, timeout);
        if (n == -1) {
            /*  If a signal is received while we are waiting,
                epoll_wait will return with an EINTR error.
                Just try again in that case.  */
            if (errno == EINTR) {
                continue;
            }
            epoll_ctl(epfd_blocking, EPOLL_CTL_DEL, g->value_fd, &ev);
            return -2;
        }
        if (initial_edge) {    // first time triggers with current state, so ignore
            initial_edge = 0;
        } else {
            gettimeofday(&tv_timenow, NULL);
            timenow = tv_timenow.tv_sec*1E6 + tv_timenow.tv_usec;
            if (g->bouncetime == NO_BOUNCETIME || timenow - g->lastcall > (unsigned int)g->bouncetime*1000 || g->lastcall == 0 || g->lastcall > timenow) {
                g->lastcall = timenow;
                finished = 1;
            }
        }
    }

    // check event was valid
    if (n > 0) {
        lseek(events.data.fd, 0, SEEK_SET);
        if ((read(events.data.fd, &buf, 1) != 1) || (events.data.fd != g->value_fd)) {
            epoll_ctl(epfd_blocking, EPOLL_CTL_DEL, g->value_fd, &ev);
            return -2;
        }
    }

    epoll_ctl(epfd_blocking, EPOLL_CTL_DEL, g->value_fd, &ev);
    if (n == 0) {
       return 0; // timeout
    } else {
       return 1; // edge found
    }
}
