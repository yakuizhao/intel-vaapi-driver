/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "host/libs/vm_manager/libvirt_manager.h"

#include <stdio.h>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <libxml/tree.h>

DEFINE_string(hypervisor_uri, "qemu:///system", "Hypervisor cannonical uri.");
DEFINE_bool(log_xml, false, "Log the XML machine configuration");

// A lot of useful information about the document created here can be found on
// these websites:
// - https://libvirt.org/formatdomain.html
// - https://wiki.libvirt.org/page/Virtio
namespace vm_manager {

namespace {
// This trivial no-op helper function serves purpose of making libxml2 happy.
// Apparently, *most* (not all!) string literals in libxml2 have to be of
// unsigned char* (aka xmlChar*) type.
inline const xmlChar* xc(const char* str) {
  return reinterpret_cast<const xmlChar*>(str);
}

// Helper functions that allow us to combine any set of arguments to a single
// string.
// Example:
//   concat("Answer", ' ', "is: ", 42);
// will produce string "Answer is: 42"
template <typename Arg>
inline std::ostream& concat_helper(std::ostream& o, Arg a) {
  o << a;
  return o;
}

template <typename Arg, typename... Args>
inline std::ostream& concat_helper(std::ostream& o, Arg a, Args... args) {
  o << a;
  concat_helper(o, args...);
  return o;
}

template <typename... Args>
inline std::string concat(Args... args) {
  std::ostringstream str;
  concat_helper(str, args...);
  return str.str();
}

enum class DeviceSourceType {
  kFile,
  kUnixSocketClient,
  kUnixSocketServer,
};

// Basic VM configuration.
// This section configures name, basic resource allocation and response to
// events.
void ConfigureVM(xmlNode* root, const std::string& instance_name, int cpus,
                 int mem_mb, const std::string& uuid) {
  xmlNewChild(root, nullptr, xc("name"), xc(instance_name.c_str()));

  // TODO(ender): should this all be 'restart'?
  xmlNewChild(root, nullptr, xc("on_poweroff"), xc("destroy"));
  xmlNewChild(root, nullptr, xc("on_reboot"), xc("restart"));
  xmlNewChild(root, nullptr, xc("on_crash"), xc("restart"));
  xmlNewChild(root, nullptr, xc("vcpu"), xc(concat(cpus).c_str()));
  xmlNewChild(root, nullptr, xc("memory"), xc(concat(mem_mb << 10).c_str()));
  if (uuid.size()) {
    xmlNewChild(root, nullptr, xc("uuid"), xc(uuid.c_str()));
  }
}

// Configure VM features.
// This section takes care of the <features> section of the target XML file.
void ConfigureVMFeatures(xmlNode* root,
                         const std::initializer_list<std::string>& features) {
  auto ch = xmlNewChild(root, nullptr, xc("features"), nullptr);
  for (const auto& str : features) {
    xmlNewChild(ch, nullptr, xc(str.c_str()), nullptr);
  }
}

// Configure VM OS.
// This section configures target os (<os>).
void ConfigureOperatingSystem(xmlNode* root, const std::string& kernel,
                              const std::string& initrd,
                              const std::string& args, const std::string& dtb) {
  auto os = xmlNewChild(root, nullptr, xc("os"), nullptr);

  auto type = xmlNewChild(os, nullptr, xc("type"), xc("hvm"));
  xmlNewProp(type, xc("arch"), xc("x86_64"));
  xmlNewProp(type, xc("machine"), xc("pc"));

  xmlNewChild(os, nullptr, xc("kernel"), xc(kernel.c_str()));
  xmlNewChild(os, nullptr, xc("initrd"), xc(initrd.c_str()));
  xmlNewChild(os, nullptr, xc("cmdline"), xc(args.c_str()));
  xmlNewChild(os, nullptr, xc("dtb"), xc(dtb.c_str()));
}

// Configure QEmu specific arguments.
// This section adds the <qemu:commandline> node.
void ConfigureQEmuSpecificOptions(
    xmlNode* root, std::initializer_list<std::string> qemu_args) {
  xmlNs* qemu_ns{xmlNewNs(
      root, xc("http://libvirt.org/schemas/domain/qemu/1.0"), xc("qemu"))};

  auto cmd = xmlNewChild(root, qemu_ns, xc("commandline"), nullptr);
  for (const auto& str : qemu_args) {
    auto arg = xmlNewChild(cmd, qemu_ns, xc("arg"), nullptr);
    xmlNewProp(arg, xc("value"), xc(str.c_str()));
  }
}

void ConfigureDeviceSource(xmlNode* device, DeviceSourceType type,
                           const std::string& path) {
  auto source = xmlNewChild(device, nullptr, xc("source"), nullptr);
  xmlNewProp(source, xc("path"), xc(path.c_str()));

  switch (type) {
    case DeviceSourceType::kFile:
      xmlNewProp(device, xc("type"), xc("file"));
      break;

    case DeviceSourceType::kUnixSocketClient:
      xmlNewProp(device, xc("type"), xc("unix"));
      xmlNewProp(source, xc("mode"), xc("connect"));
      break;

    case DeviceSourceType::kUnixSocketServer:
      xmlNewProp(device, xc("type"), xc("unix"));
      xmlNewProp(source, xc("mode"), xc("bind"));
      break;
  }
}

// Configure serial port.
// This section adds <serial> elements to <device> node.
void ConfigureSerialPort(xmlNode* devices, int port, DeviceSourceType type,
                         const std::string& path) {
  auto tty = xmlNewChild(devices, nullptr, xc("serial"), nullptr);
  ConfigureDeviceSource(tty, type, path);

  if (type == DeviceSourceType::kFile) {
    LOG(INFO) << "Non-interactive serial port will send output to " << path;
  } else {
    LOG(INFO) << "Interactive serial port set up. To access the console run:";
    LOG(INFO) << "$ sudo socat file:$(tty),raw,echo=0 " << path;
  }
  auto tgt = xmlNewChild(tty, nullptr, xc("target"), nullptr);
  xmlNewProp(tgt, xc("port"), xc(concat(port).c_str()));
}

// Configure disk partition.
// This section adds <disk> elements to <devices> node.
void ConfigureDisk(xmlNode* devices, const std::string& name,
                   const std::string& path) {
  auto ch = xmlNewChild(devices, nullptr, xc("disk"), nullptr);
  xmlNewProp(ch, xc("type"), xc("file"));

  auto dr = xmlNewChild(ch, nullptr, xc("driver"), nullptr);
  xmlNewProp(dr, xc("name"), xc("qemu"));
  xmlNewProp(dr, xc("type"), xc("raw"));
  xmlNewProp(dr, xc("io"), xc("threads"));

  auto tg = xmlNewChild(ch, nullptr, xc("target"), nullptr);
  xmlNewProp(tg, xc("dev"), xc(name.c_str()));
  xmlNewProp(tg, xc("bus"), xc("virtio"));

  auto sr = xmlNewChild(ch, nullptr, xc("source"), nullptr);
  xmlNewProp(sr, xc("file"), xc(path.c_str()));
}

// Configure virtio channel.
// This section adds <channel> elements to <devices> node.
void ConfigureVirtioChannel(xmlNode* devices, int port, const std::string& name,
                            DeviceSourceType type, const std::string& path) {
  if (path.empty()) {
    return;
  }
  auto vch = xmlNewChild(devices, nullptr, xc("channel"), nullptr);
  ConfigureDeviceSource(vch, type, path);

  auto tgt = xmlNewChild(vch, nullptr, xc("target"), nullptr);
  xmlNewProp(tgt, xc("type"), xc("virtio"));
  xmlNewProp(tgt, xc("name"), xc(name.c_str()));

  auto adr = xmlNewChild(vch, nullptr, xc("address"), nullptr);
  xmlNewProp(adr, xc("type"), xc("virtio-serial"));
  xmlNewProp(adr, xc("controller"), xc("0"));
  xmlNewProp(adr, xc("bus"), xc("0"));
  xmlNewProp(adr, xc("port"), xc(concat(port).c_str()));
}

// Configure network interface.
// This section adds <interface> elements to <devices> node.
void ConfigureNIC(xmlNode* devices, const std::string& name,
                  const std::string& bridge, int guest_id, int nic_id) {
  auto nic = xmlNewChild(devices, nullptr, xc("interface"), nullptr);
  xmlNewProp(nic, xc("type"), xc("bridge"));

  auto brg = xmlNewChild(nic, nullptr, xc("source"), nullptr);
  xmlNewProp(brg, xc("bridge"), xc(bridge.c_str()));

  auto mac = xmlNewChild(nic, nullptr, xc("mac"), nullptr);
  xmlNewProp(mac, xc("address"),
             xc(concat("00:43:56:44:", std::setfill('0'), std::hex,
                       std::setw(2), guest_id, ':', std::setw(2), nic_id)
                    .c_str()));

  auto mdl = xmlNewChild(nic, nullptr, xc("model"), nullptr);
  xmlNewProp(mdl, xc("type"), xc("virtio"));

  auto tgt = xmlNewChild(nic, nullptr, xc("target"), nullptr);
  xmlNewProp(tgt, xc("dev"), xc(name.c_str()));
}

// Configure Harwdare Random Number Generator.
// This section adds <rng> element to <devices> node.
void ConfigureHWRNG(xmlNode* devices, const std::string& entsrc) {
  auto rng = xmlNewChild(devices, nullptr, xc("rng"), nullptr);
  xmlNewProp(rng, xc("model"), xc("virtio"));

  auto rate = xmlNewChild(rng, nullptr, xc("rate"), nullptr);
  xmlNewProp(rate, xc("period"), xc("2000"));
  xmlNewProp(rate, xc("bytes"), xc("1024"));

  auto bend = xmlNewChild(rng, nullptr, xc("backend"), xc(entsrc.c_str()));
  xmlNewProp(bend, xc("model"), xc("random"));
}

std::string GetLibvirtCommand() {
  std::string cmd = "virsh";
  if (!FLAGS_hypervisor_uri.empty()) {
    cmd += " -c " + FLAGS_hypervisor_uri;
  }
  return cmd;
}

}  // namespace

std::string LibvirtManager::BuildXmlConfig() const {
  auto config = vsoc::CuttlefishConfig::Get();
  std::string instance_name = config->instance_name();

  std::unique_ptr<xmlDoc, void (*)(xmlDocPtr)> xml{xmlNewDoc(xc("1.0")),
                                                   xmlFreeDoc};
  auto root{xmlNewNode(nullptr, xc("domain"))};
  xmlDocSetRootElement(xml.get(), root);
  xmlNewProp(root, xc("type"), xc("kvm"));

  ConfigureVM(root, instance_name, config->cpus(), config->memory_mb(),
              config->uuid());
  ConfigureVMFeatures(root, {"acpi", "apic", "hap"});
  ConfigureOperatingSystem(root, config->kernel_image_path(),
                           config->ramdisk_image_path(), config->kernel_args(),
                           config->dtb_path());
  ConfigureQEmuSpecificOptions(
      root, {"-chardev",
             concat("socket,path=", config->ivshmem_qemu_socket_path(),
                    ",id=ivsocket"),
             "-device",
             concat("ivshmem-doorbell,chardev=ivsocket,vectors=",
                    config->ivshmem_vector_count()),
             "-cpu", "host"});

  if (config->disable_app_armor_security()) {
    auto seclabel = xmlNewChild(root, nullptr, xc("seclabel"), nullptr);
    xmlNewProp(seclabel, xc("type"), xc("none"));
    xmlNewProp(seclabel, xc("model"), xc("apparmor"));
  }
  if (config->disable_dac_security()) {
    auto seclabel = xmlNewChild(root, nullptr, xc("seclabel"), nullptr);
    xmlNewProp(seclabel, xc("type"), xc("none"));
    xmlNewProp(seclabel, xc("model"), xc("dac"));
  }

  auto devices = xmlNewChild(root, nullptr, xc("devices"), nullptr);

  ConfigureSerialPort(devices, 0, DeviceSourceType::kUnixSocketClient,
                      config->kernel_log_socket_name());
  ConfigureSerialPort(devices, 1, DeviceSourceType::kUnixSocketServer,
                      config->console_path());
  ConfigureVirtioChannel(devices, 1, "cf-logcat", DeviceSourceType::kFile,
                         config->logcat_path());
  ConfigureVirtioChannel(devices, 2, "cf-gadget-usb-v1",
                         DeviceSourceType::kUnixSocketClient,
                         config->usb_v1_socket_name());

  ConfigureDisk(devices, "vda", config->system_image_path());
  ConfigureDisk(devices, "vdb", config->data_image_path());
  ConfigureDisk(devices, "vdc", config->cache_image_path());
  ConfigureDisk(devices, "vdd", config->vendor_image_path());

  ConfigureNIC(devices, config->mobile_tap_name(), config->mobile_bridge_name(),
               vsoc::GetInstance(), 1);
  ConfigureHWRNG(devices, config->entropy_source());

  xmlChar* tgt;
  int tgt_len;

  xmlDocDumpFormatMemoryEnc(xml.get(), &tgt, &tgt_len, "utf-8", true);
  std::string out((const char*)(tgt), tgt_len);
  xmlFree(tgt);
  return out;
}

bool LibvirtManager::Start() const {
  std::string start_command = GetLibvirtCommand();
  start_command += " create /dev/fd/0";

  std::string xml = BuildXmlConfig();
  if (FLAGS_log_xml) {
    LOG(INFO) << "Using XML:\n" << xml;
  }

  FILE* launch = popen(start_command.c_str(), "w");
  if (!launch) {
    LOG(FATAL) << "Unable to execute " << start_command;
    return false;
  }
  int rval = fputs(xml.c_str(), launch);
  if (rval == EOF) {
    LOG(FATAL) << "Launch command exited while accepting XML";
    return false;
  }
  int exit_code = pclose(launch);
  if (exit_code != 0) {
    LOG(FATAL) << "Launch command exited with status " << exit_code;
    return false;
  }
  return true;
}

bool LibvirtManager::Stop() const {
  auto config = vsoc::CuttlefishConfig::Get();
  auto stop_command = GetLibvirtCommand();
  stop_command += " destroy " + config->instance_name();
  return std::system(stop_command.c_str()) == 0;
}

}  // namespace vm_manager
