// See LICENSE for license details.

#include "htif.h"
#include "rfb.h"
#include "elfloader.h"
#include "encoding.h"
#include <algorithm>
#include <assert.h>
#include <vector>
#include <queue>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

/* Attempt to determine the execution prefix automatically.  autoconf
 * sets PREFIX, and pconfigure sets __PCONFIGURE__PREFIX. */
#if !defined(PREFIX) && defined(__PCONFIGURE__PREFIX)
# define PREFIX __PCONFIGURE__PREFIX
#endif

#ifndef TARGET_ARCH
# define TARGET_ARCH "riscv64-unknown-elf"
#endif

#ifndef TARGET_DIR
# define TARGET_DIR "/" TARGET_ARCH "/bin/"
#endif

static volatile bool signal_exit = false;
static void handle_signal(int sig)
{
  if (sig == SIGABRT || signal_exit) // someone set up us the bomb!
    exit(-1);
  signal_exit = true;
  signal(sig, &handle_signal);
}

htif_t::htif_t()
  : mem(this), entry(DRAM_BASE), sig_addr(0), sig_len(0),
    tohost_addr(0), fromhost_addr(0), exitcode(0), stopped(false),
    syscall_proxy(this)
{
  signal(SIGINT, &handle_signal);
  signal(SIGTERM, &handle_signal);
  signal(SIGABRT, &handle_signal); // we still want to call static destructors
}

htif_t::htif_t(int argc, char** argv) : htif_t()
{
  parse_arguments(argc, argv);
  register_devices();
}

htif_t::htif_t(const std::vector<std::string>& args) : htif_t()
{
  int argc = args.size() + 1;
  char * argv[argc];
  argv[0] = (char *) "htif";
  for (unsigned int i = 0; i < args.size(); i++) {
    argv[i+1] = (char *) args[i].c_str();
  }

  parse_arguments(argc, argv);
  register_devices();
}

htif_t::~htif_t()
{
  for (auto d : dynamic_devices)
    delete d;
}

void htif_t::start()
{
  if (!targs.empty() && targs[0] != "none")
      load_program();

  reset();
}

void htif_t::load_program()
{
  std::string path;
  if (access(targs[0].c_str(), F_OK) == 0)
    path = targs[0];
  else if (targs[0].find('/') == std::string::npos)
  {
    std::string test_path = PREFIX TARGET_DIR + targs[0];
    if (access(test_path.c_str(), F_OK) == 0)
      path = test_path;
  }

  if (path.empty())
    throw std::runtime_error("could not open " + targs[0]);

  std::map<std::string, uint64_t> symbols = load_elf(path.c_str(), &mem, &entry);

  if (symbols.count("tohost") && symbols.count("fromhost")) {
    tohost_addr = symbols["tohost"];
    fromhost_addr = symbols["fromhost"];
  } else {
    fprintf(stderr, "warning: tohost and fromhost symbols not in ELF; can't communicate with target\n");
  }

  // detect torture tests so we can print the memory signature at the end
  if (symbols.count("begin_signature") && symbols.count("end_signature"))
  {
    sig_addr = symbols["begin_signature"];
    sig_len = symbols["end_signature"] - sig_addr;
  }
}

void htif_t::stop()
{
  if (!sig_file.empty() && sig_len) // print final torture test signature
  {
    std::vector<uint8_t> buf(sig_len);
    mem.read(sig_addr, sig_len, &buf[0]);

    std::ofstream sigs(sig_file);
    assert(sigs && "can't open signature file!");
    sigs << std::setfill('0') << std::hex;

    const addr_t incr = 16;
    assert(sig_len % incr == 0);
    for (addr_t i = 0; i < sig_len; i += incr)
    {
      for (addr_t j = incr; j > 0; j--)
        sigs << std::setw(2) << (uint16_t)buf[i+j-1];
      sigs << '\n';
    }

    sigs.close();
  }

  stopped = true;
}

void htif_t::clear_chunk(addr_t taddr, size_t len)
{
  char zeros[chunk_max_size()];
  memset(zeros, 0, chunk_max_size());

  for (size_t pos = 0; pos < len; pos += chunk_max_size())
    write_chunk(taddr + pos, std::min(len - pos, chunk_max_size()), zeros);
}

int htif_t::run()
{
  start();

  auto enq_func = [](std::queue<reg_t>* q, uint64_t x) { q->push(x); };
  std::queue<reg_t> fromhost_queue;
  std::function<void(reg_t)> fromhost_callback =
    std::bind(enq_func, &fromhost_queue, std::placeholders::_1);

  if (tohost_addr == 0) {
    while (true)
      idle();
  }

  while (!signal_exit && exitcode == 0)
  {
    if (auto tohost = mem.read_uint64(tohost_addr)) {
      mem.write_uint64(tohost_addr, 0);
      command_t cmd(this, tohost, fromhost_callback);
      device_list.handle_command(cmd);
    } else {
      idle();
    }

    device_list.tick();

    if (!fromhost_queue.empty() && mem.read_uint64(fromhost_addr) == 0) {
      mem.write_uint64(fromhost_addr, fromhost_queue.front());
      fromhost_queue.pop();
    }
  }

  stop();

  return exit_code();
}

bool htif_t::done()
{
  return stopped;
}

int htif_t::exit_code()
{
  return exitcode >> 1;
}

void htif_t::parse_arguments(int argc, char ** argv)
{
  optind = 0; // reset optind as HTIF may run getopt _after_ others
  while (1) {
    static struct option long_options[] = { HTIF_LONG_OPTIONS };
    int option_index = 0;
    int c = getopt_long(argc, argv, "-c:", long_options, &option_index);

    if (c == -1) break;
 retry:
    switch (c) {
      case '?': break;
      case HTIF_LONG_OPTIONS_OPTIND:
        if (optarg) dynamic_devices.push_back(new rfb_t(atoi(optarg)));
        else        dynamic_devices.push_back(new rfb_t);
        break;
      case HTIF_LONG_OPTIONS_OPTIND + 1:
        // [TODO] Remove once disks are supported again
        throw std::invalid_argument("--disk/+disk unsupported (use a ramdisk)");
        dynamic_devices.push_back(new disk_t(optarg));
        break;
      case HTIF_LONG_OPTIONS_OPTIND + 2:
        sig_file = optarg;
        break;
      case HTIF_LONG_OPTIONS_OPTIND + 3:
        syscall_proxy.set_chroot(optarg);
        break;
      case 1: {
        std::string arg = optarg;
        if (arg == "+rfb") {
          c = HTIF_LONG_OPTIONS_OPTIND;
          optarg = nullptr;
        }
        else if (arg.find("+rfb=") == 0) {
          c = HTIF_LONG_OPTIONS_OPTIND;
          optarg = optarg + 5;
        }
        else if (arg.find("+disk=") == 0) {
          c = HTIF_LONG_OPTIONS_OPTIND + 1;
          optarg = optarg + 6;
        }
        else if (arg.find("+signature=") == 0) {
          c = HTIF_LONG_OPTIONS_OPTIND + 2;
          optarg = optarg + 11;
        }
        else if (arg.find("+chroot=") == 0) {
          c = HTIF_LONG_OPTIONS_OPTIND + 3;
          optarg = optarg + 8;
        }
        else {
          if(arg.find("+") != 0) {
            optind--;
            goto done_processing;
          }
          break;
        }
        goto retry;
      }
        // Special case non-standard VCS options that should be ignored
      case 'c': // e.g., '-cm line+cond'
        if (!strcmp(optarg, "m"))
          optind++;
        else
          throw std::invalid_argument("Expected 'm' to follow VCS special case '-c' option");
    }
  }

done_processing:
  while (optind < argc)
    targs.push_back(argv[optind++]);
  if (!targs.size()) {
    throw std::invalid_argument("No binary specified for host");
  }
}

void htif_t::register_devices()
{
  device_list.register_device(&syscall_proxy);
  device_list.register_device(&bcd);
  for (auto d : dynamic_devices)
    device_list.register_device(d);
}
