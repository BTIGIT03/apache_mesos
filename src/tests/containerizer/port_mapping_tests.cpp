// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/mac.hpp>
#include <stout/net.hpp>
#include <stout/stopwatch.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/stat.hpp>

#include "common/values.hpp"

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "linux/routing/utils.hpp"

#include "linux/routing/filter/ip.hpp"

#include "linux/routing/link/link.hpp"

#include "linux/routing/queueing/ingress.hpp"

#include "master/master.hpp"

#include "mesos/mesos.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/launcher.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "slave/containerizer/mesos/isolators/network/port_mapping.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::slave;

using namespace process;

using namespace routing;
using namespace routing::filter;
using namespace routing::queueing;

using mesos::internal::master::Master;

using mesos::internal::values::rangesToIntervalSet;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::ContainerTermination;
using mesos::slave::Isolator;

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::Eq;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


// An old glibc might not have this symbol.
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif


// Each test container works with a common specification of 2 CPUs,
// 1GB of memory and 1GB of disk space, which experience has shown
// to be sufficient to not encounter resource starvation issues when
// running the test suite.
const char* const containerCPU = "cpus:2";
const char* const containerMemory = "mem:1024";
const char* const containerDisk = "disk:1024";

// We configure ephemeral and persistent port ranges outside the
// default linux ip_local_port_range [32768-61000] in order to reduce
// the probability of a conflict which could result in spurious
// results (positive or negative) from these tests.
const char* const ephemeralPorts = "ephemeral_ports:[30001-30999]";
const char* const persistentPorts = "ports:[31000-32000]";

// To keep things simple, we used fixed port ranges for our containers
// in these tests rather than try to dynamically track port usage.
// Note that container ports must be contained in the persistent port
// range.
const char* const container1Ports = "ports:[31000-31499]";
const char* const container2Ports = "ports:[31500-32000]";

// We define a validPort in the container1 assigned range which can
// therefore accept incoming traffic.
const int validPort = 31001;

// We also define a port outside the persistent port range; containers
// connecting to this port will never receive incoming traffic.
const int invalidPort = 32502;


static void cleanup(const string& eth0, const string& lo)
{
  // Clean up the ingress qdisc on eth0 and lo if exists.
  Try<bool> hostEth0ExistsQdisc = ingress::exists(eth0);
  ASSERT_SOME(hostEth0ExistsQdisc);

  if (hostEth0ExistsQdisc.get()) {
    ASSERT_SOME_TRUE(ingress::remove(eth0));
  }

  Try<bool> hostLoExistsQdisc = ingress::exists(lo);
  ASSERT_SOME(hostLoExistsQdisc);

  if (hostLoExistsQdisc.get()) {
    ASSERT_SOME_TRUE(ingress::remove(lo));
  }

  // Clean up all 'veth' devices if exist.
  Try<set<string>> links = net::links();
  ASSERT_SOME(links);

  foreach (const string& name, links.get()) {
    if (strings::startsWith(name, slave::PORT_MAPPING_VETH_PREFIX())) {
      ASSERT_SOME_TRUE(link::remove(name));
    }
  }

  if (os::exists(slave::PORT_MAPPING_BIND_MOUNT_ROOT())) {
    Try<list<string>> entries = os::ls(slave::PORT_MAPPING_BIND_MOUNT_ROOT());
    ASSERT_SOME(entries);

    foreach (const string& file, entries.get()) {
      string target = path::join(slave::PORT_MAPPING_BIND_MOUNT_ROOT(), file);

      // NOTE: Here, we ignore the unmount errors because previous tests
      // may have created the file and died before mounting.
      if (!os::stat::islink(target)) {
        mesos::internal::fs::unmount(target, MNT_DETACH);
      }

      // Remove the network namespace handle and the corresponding
      // symlinks. The removal here is best effort.
      os::rm(target);
    }
  }
}


class PortMappingIsolatorTest : public TemporaryDirectoryTest
{
public:
  static void SetUpTestCase()
  {
    ASSERT_SOME(routing::check())
      << "-------------------------------------------------------------\n"
      << "We cannot run any PortMappingIsolatorTests because your\n"
      << "libnl library is not new enough. You can either install a\n"
      << "new libnl library, or disable this test case\n"
      << "-------------------------------------------------------------";

    ASSERT_SOME(os::shell("which nc"))
      << "-------------------------------------------------------------\n"
      << "We cannot run any PortMappingIsolatorTests because 'nc'\n"
      << "could not be found. You can either install 'nc', or disable\n"
      << "this test case\n"
      << "-------------------------------------------------------------";

    ASSERT_SOME(os::shell("which arping"))
      << "-------------------------------------------------------------\n"
      << "We cannot run some PortMappingIsolatorTests because 'arping'\n"
      << "could not be found. You can either install 'arping', or\n"
      << "disable this test case\n"
      << "-------------------------------------------------------------";
  }

  PortMappingIsolatorTest() : hostIP(net::IP(INADDR_ANY)) {}

protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    flags = CreateSlaveFlags();

    // Guess the name of the public interface.
    Result<string> _eth0 = link::eth0();
    ASSERT_SOME(_eth0) << "Failed to guess the name of the public interface";

    eth0 = _eth0.get();

    LOG(INFO) << "Using " << eth0 << " as the public interface";

    // Guess the name of the loopback interface.
    Result<string> _lo = link::lo();
    ASSERT_SOME(_lo) << "Failed to guess the name of the loopback interface";

    lo = _lo.get();

    LOG(INFO) << "Using " << lo << " as the loopback interface";

    // Clean up qdiscs and veth devices.
    cleanup(eth0, lo);

    // Get host IP address.
    Result<net::IP::Network> hostIPNetwork =
        net::IP::Network::fromLinkDevice(eth0, AF_INET);

    ASSERT_SOME(hostIPNetwork)
      << "Failed to retrieve the host public IP network from " << eth0 << ": "
      << hostIPNetwork.error();

    hostIP = hostIPNetwork->address();

    // Get all the external name servers for tests that need to talk
    // to an external host, e.g., ping, DNS.
    Try<string> read = os::read("/etc/resolv.conf");
    ASSERT_SOME(read);

    foreach (const string& line, strings::split(read.get(), "\n")) {
      if (!strings::startsWith(line, "nameserver")) {
        continue;
      }

      vector<string> tokens = strings::split(line, " ");
      ASSERT_LE(2u, tokens.size()) << "Unexpected format in '/etc/resolv.conf'";
      if (tokens[1] != "127.0.0.1") {
        nameServers.push_back(tokens[1]);
      }
    }

    container1Ready = path::join(os::getcwd(), "container1_ready");
    container2Ready = path::join(os::getcwd(), "container2_ready");
    trafficViaLoopback = path::join(os::getcwd(), "traffic_via_loopback");
    trafficViaPublic = path::join(os::getcwd(), "traffic_via_public");
    exitStatus = path::join(os::getcwd(), "exit_status");
  }

  virtual void TearDown()
  {
    cleanup(eth0, lo);
    TemporaryDirectoryTest::TearDown();
  }

  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags;

    flags.launcher_dir = getLauncherDir();

    flags.resources = strings::join(";", vector<string>({
        containerCPU,
        containerMemory,
        containerDisk,
        ephemeralPorts,
        persistentPorts }));

    // NOTE: '16' should be enough for all our tests.
    flags.ephemeral_ports_per_container = 16;

    flags.isolation = "network/port_mapping";

    return flags;
  }

  Try<pid_t> launchHelper(
      Launcher* launcher,
      int pipes[2],
      const ContainerID& containerId,
      const string& command,
      const Option<ContainerLaunchInfo>& isolatorLaunchInfo)
  {
    CommandInfo commandInfo;
    commandInfo.set_value(command);

    // The flags to pass to the helper process.
    MesosContainerizerLaunch::Flags launchFlags;

    launchFlags.pipe_read = pipes[0];
    launchFlags.pipe_write = pipes[1];

    ContainerLaunchInfo launchInfo;
    launchInfo.mutable_command()->CopyFrom(commandInfo);
    launchInfo.set_working_directory(os::getcwd());

    Result<string> user = os::user();
    if (user.isError()) {
      return Error(user.error());
    } else if (user.isNone()) {
      return Error("Could not get current user");
    }
    launchInfo.set_user(user.get());

    if (isolatorLaunchInfo.isSome()) {
      launchInfo.mutable_pre_exec_commands()->MergeFrom(
          isolatorLaunchInfo->pre_exec_commands());
    }

    launchFlags.launch_info = JSON::protobuf(launchInfo);

    vector<string> argv(2);
    argv[0] = MESOS_CONTAINERIZER;
    argv[1] = MesosContainerizerLaunch::NAME;

    Try<pid_t> pid = launcher->fork(
        containerId,
        path::join(flags.launcher_dir, MESOS_CONTAINERIZER),
        argv,
        mesos::slave::ContainerIO(),
        &launchFlags,
        None(),
        None(),
        CLONE_NEWNET | CLONE_NEWNS,
        {pipes[0], pipes[1]});

    return pid;
  }

  Result<ResourceStatistics> statisticsHelper(
      pid_t pid,
      bool enable_summary,
      bool enable_details,
      bool enable_snmp)
  {
    // Retrieve the socket information from inside the container.
    PortMappingStatistics statistics;
    statistics.flags.pid = pid;
    statistics.flags.eth0_name = eth0;
    statistics.flags.enable_socket_statistics_summary = enable_summary;
    statistics.flags.enable_socket_statistics_details = enable_details;
    statistics.flags.enable_snmp_statistics = enable_snmp;

    vector<string> argv(2);
    argv[0] = "mesos-network-helper";
    argv[1] = PortMappingStatistics::NAME;

    // We don't need STDIN; we need STDOUT for the result; we leave
    // STDERR as is to log to slave process.
    Try<Subprocess> s = subprocess(
        path::join(flags.launcher_dir, "mesos-network-helper"),
        argv,
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::PIPE(),
        Subprocess::FD(STDERR_FILENO),
        &statistics.flags);

    if (s.isError()) {
      return Error(s.error());
    }

    Future<Option<int>> status = s->status();
    AWAIT_EXPECT_READY(status);
    EXPECT_SOME_EQ(0, status.get());

    Future<string> out = io::read(s->out().get());
    AWAIT_EXPECT_READY(out);

    Try<JSON::Object> object = JSON::parse<JSON::Object>(out.get());

    if (object.isError()) {
      return Error(object.error());
    }

    return ::protobuf::parse<ResourceStatistics>(object.get());
  }

  slave::Flags flags;

  // Name of the host eth0 and lo.
  string eth0;
  string lo;

  // Host public IP network.
  net::IP hostIP;

  // All the external name servers as read from /etc/resolv.conf.
  vector<string> nameServers;

  // Some auxiliary files for the tests.
  string container1Ready;
  string container2Ready;
  string trafficViaLoopback;
  string trafficViaPublic;
  string exitStatus;
};


// Wait up to timeout seconds for a file to be created. If timeout is
// zero, then wait indefinitely. Return true if file exists.
//
// TODO(pbrett): Consider generalizing this function and moving it to
// a common header.
static bool waitForFileCreation(
    const string& path,
    const Duration& duration = Seconds(60))
{
  Stopwatch timer;
  timer.start();
  while (!os::exists(path)) {
    if ((duration > Duration::zero()) && (timer.elapsed() > duration))
      break;
    os::sleep(Milliseconds(50));
  }
  return os::exists(path);
}


// This test uses two containers: one listens to 'validPort' and
// 'invalidPort' and writes data received to files; the other
// container attempts to connect to the previous container using
// 'validPort' and 'invalidPort'. Verify that only the connection
// through 'validPort' is successful by confirming that the expected
// data has been written to its output file.
TEST_F(PortMappingIsolatorTest, ROOT_NC_ContainerToContainerTCP)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig1.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig1.set_directory(dir1.get());

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(
        containerId1,
        containerConfig1);

  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  ostringstream command1;

  // Listen to 'localhost' and 'port'.
  command1 << "nc -l localhost " << validPort << " > " << trafficViaLoopback
           << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -l " << hostIP << " " << validPort << " > "
           << trafficViaPublic << "& ";

  // Listen to 'invalidPort'.  This should not receive any data.
  command1 << "nc -l " << invalidPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  ContainerID containerId2;
  containerId2.set_value(id::UUID::random().toString());

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  ContainerConfig containerConfig2;
  containerConfig2.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig2.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig2.set_directory(dir2.get());

  Future<Option<ContainerLaunchInfo>> launchInfo2 =
    isolator.get()->prepare(
        containerId2,
        containerConfig2);

  AWAIT_READY(launchInfo2);
  ASSERT_SOME(launchInfo2.get());
  ASSERT_EQ(1, launchInfo2.get()->pre_exec_commands().size());

  ostringstream command2;

  // Send to 'localhost' and 'port'.
  command2 << "printf hello1 | nc localhost " << validPort << ";";
  // Send to 'localhost' and 'invalidPort'. This should fail.
  command2 << "printf hello2 | nc localhost " << invalidPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "printf hello3 | nc " << hostIP << " " << validPort << ";";
  // Send to 'public IP' and 'invalidPort'. This should fail.
  command2 << "printf hello4 | nc " << hostIP << " " << invalidPort << ";";
  // Touch the guard file.
  command2 << "touch " << container2Ready;

  ASSERT_NE(-1, ::pipe(pipes));

  pid = launchHelper(
      launcher.get(),
      pipes,
      containerId2,
      command2.str(),
      launchInfo2.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status2 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId2, pid.get()));

  // Now signal the child to continue.
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  ASSERT_TRUE(waitForFileCreation(container2Ready));

  // Wait for the command to complete.
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(launcher.get()->destroy(containerId2));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId2));

  delete isolator.get();
  delete launcher.get();
}


// The same container-to-container test but with UDP.
TEST_F(PortMappingIsolatorTest, ROOT_NC_ContainerToContainerUDP)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig1.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig1.set_directory(dir1.get());

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(
        containerId1,
        containerConfig1);

  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  ostringstream command1;

  // Listen to 'localhost' and 'port'.
  command1 << "nc -u -l localhost " << validPort << " > " << trafficViaLoopback
           << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -u -l " << hostIP << " " << validPort << " > "
           << trafficViaPublic << "& ";

  // Listen to 'invalidPort'. This should not receive anything.
  command1 << "nc -u -l " << invalidPort << " | tee " << trafficViaLoopback
           << " " << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  ContainerID containerId2;
  containerId2.set_value(id::UUID::random().toString());

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  ContainerConfig containerConfig2;
  containerConfig2.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig2.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig2.set_directory(dir2.get());

  Future<Option<ContainerLaunchInfo>> launchInfo2 =
    isolator.get()->prepare(
        containerId2,
        containerConfig2);

  AWAIT_READY(launchInfo2);
  ASSERT_SOME(launchInfo2.get());
  ASSERT_EQ(1, launchInfo2.get()->pre_exec_commands().size());

  ostringstream command2;

  // Send to 'localhost' and 'port'.
  command2 << "printf hello1 | nc -w1 -u localhost " << validPort << ";";
  // Send to 'localhost' and 'invalidPort'. No data should be sent.
  command2 << "printf hello2 | nc -w1 -u localhost " << invalidPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "printf hello3 | nc -w1 -u " << hostIP << " " << validPort << ";";
  // Send to 'public IP' and 'invalidPort'. No data should be sent.
  command2 << "printf hello4 | nc -w1 -u " << hostIP << " " << invalidPort
           << ";";
  // Touch the guard file.
  command2 << "touch " << container2Ready;

  ASSERT_NE(-1, ::pipe(pipes));

  pid = launchHelper(
      launcher.get(),
      pipes,
      containerId2,
      command2.str(),
      launchInfo2.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status2 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId2, pid.get()));

  // Now signal the child to continue.
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  ASSERT_TRUE(waitForFileCreation(container2Ready));

  // Wait for the command to complete.
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(launcher.get()->destroy(containerId2));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId2));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a UDP server is in a container while host
// tries to establish a UDP connection.
TEST_F(PortMappingIsolatorTest, ROOT_NC_HostToContainerUDP)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream command1;

  // Listen to 'localhost' and 'Port'.
  command1 << "nc -u -l localhost " << validPort << " > " << trafficViaLoopback
           << "& ";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -u -l " << hostIP << " " << validPort << " > "
           << trafficViaPublic << "& ";

  // Listen to 'public IP' and 'invalidPort'. This should not receive anything.
  command1 << "nc -u -l " << invalidPort << " | tee " << trafficViaLoopback
           << " " << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  // Send to 'localhost' and 'port'.
  ostringstream command2;
  command2 << "printf hello1 | nc -w1 -u localhost " << validPort;
  ASSERT_SOME(os::shell(command2.str()));

  // Send to 'localhost' and 'invalidPort'. Some 'nc' implementations
  // (e.g. Ncat from Nmap) return an error when they see ECONNREFUSED
  // on a datagram socket, so we don't check the exit code when
  // sending to 'invalidPort'.
  ostringstream command3;
  command3 << "printf hello2 | nc -w1 -u localhost " << invalidPort;
  os::shell(command3.str());

  // Send to 'public IP' and 'port'.
  ostringstream command4;
  command4 << "printf hello3 | nc -w1 -u " << hostIP << " " << validPort;
  ASSERT_SOME(os::shell(command4.str()));

  // Send to 'public IP' and 'invalidPort'.
  ostringstream command5;
  command5 << "printf hello4 | nc -w1 -u " << hostIP << " " << invalidPort;
  os::shell(command5.str());

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a TCP server is in a container while host
// tries to establish a TCP connection.
TEST_F(PortMappingIsolatorTest, ROOT_NC_HostToContainerTCP)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream command1;

  // Listen to 'localhost' and 'Port'.
  command1 << "nc -l localhost " << validPort << " > " << trafficViaLoopback
           << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -l " << hostIP << " " << validPort << " > "
           << trafficViaPublic << "&";

  // Listen to 'public IP' and 'invalidPort'. This should fail.
  command1 << "nc -l " << invalidPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "&";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  // Send to 'localhost' and 'port'.
  ostringstream command2;
  command2 << "printf hello1 | nc localhost " << validPort;
  ASSERT_SOME(os::shell(command2.str()));

  // Send to 'localhost' and 'invalidPort'. This should fail because TCP
  // connection couldn't be established..
  ostringstream command3;
  command3 << "printf hello2 | nc localhost " << invalidPort;
  ASSERT_ERROR(os::shell(command3.str()));

  // Send to 'public IP' and 'port'.
  ostringstream command4;
  command4 << "printf hello3 | nc " << hostIP << " " << validPort;
  ASSERT_SOME(os::shell(command4.str()));

  // Send to 'public IP' and 'invalidPort'. This should fail because TCP
  // connection couldn't be established.
  ostringstream command5;
  command5 << "printf hello4 | nc " << hostIP << " " << invalidPort;
  ASSERT_ERROR(os::shell(command5.str()));

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ICMP requests to
// external hosts.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerICMPExternal)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream command;
  for (size_t i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    command << "ping -c1 " << IP;
    if (i + 1 < nameServers.size()) {
      command << " && ";
    }
  }
  command << "; printf $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ICMP requests to itself.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerICMPInternal)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream command;
  command << "ping -c1 127.0.0.1 && ping -c1 " << hostIP
          << "; printf $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ARP requests to
// external hosts.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerARPExternal)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream command;
  for (size_t i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    // Time out after 1s and terminate upon receiving the first reply.
    command << "arping -f -w1 " << IP << " -I " << eth0;
    if (i + 1 < nameServers.size()) {
      command << " && ";
    }
  }
  command << "; printf $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test DNS connectivity.
TEST_F(PortMappingIsolatorTest, ROOT_DNS)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream command;
  for (size_t i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    command << "host " << IP;
    if (i + 1 < nameServers.size()) {
      command << " && ";
    }
  }
  command << "; printf $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container has run out of ephemeral ports
// to use.
TEST_F(PortMappingIsolatorTest, ROOT_TooManyContainers)
{
  // Increase the ephemeral ports per container so that we dont have
  // enough ephemeral ports to launch a second container.
  flags.ephemeral_ports_per_container = 512;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig1.set_directory(dir1.get());

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(
        containerId1,
        containerConfig1);

  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  ostringstream command1;
  command1 << "sleep 1000";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      launchInfo1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  ContainerID containerId2;
  containerId2.set_value(id::UUID::random().toString());

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  ContainerConfig containerConfig2;
  containerConfig2.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig2.set_directory(dir2.get());

  Future<Option<ContainerLaunchInfo>> launchInfo2 =
    isolator.get()->prepare(
        containerId2,
        containerConfig2);

  AWAIT_FAILED(launchInfo2);

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where PortMappingIsolator uses a very small
// egress rate limit.
TEST_F(PortMappingIsolatorTest, ROOT_NC_SmallEgressLimit)
{
  // Note that the underlying rate limiting mechanism usually has a
  // small allowance for burst. Empirically, as least 10x of the rate
  // limit amount of data is required to make sure the burst is an
  // insignificant factor of the transmission time.

  // To-be-tested egress rate limit, in Bytes/s.
  const Bytes rate = 2000;
  // Size of the data to send, in Bytes.
  const Bytes size = 20480;

  // Use a very small egress limit.
  flags.egress_rate_limit_per_container = rate;
  flags.minimum_egress_rate_limit = 0;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Open an nc server on the host side. Note that 'invalidPort' is in
  // neither 'ports' nor 'ephemeral_ports', which makes it a good port
  // to use on the host.
  ostringstream command1;
  command1 << "nc -l localhost " << invalidPort << " > " << os::DEV_NULL;
  Try<Subprocess> s = subprocess(command1.str().c_str());
  ASSERT_SOME(s);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  // Fill 'size' bytes of data. The actual content does not matter.
  string data(size.bytes(), 'a');

  ostringstream command2;
  const string transmissionTime = path::join(os::getcwd(), "transmission_time");

  command2 << "echo 'Sending " << size.bytes()
           << " bytes of data under egress rate limit " << rate.bytes()
           << "Bytes/s...';";

  command2 << "{ echo " << data << " | time -p nc localhost "
           << invalidPort << " ; } 2> " << transmissionTime << " && ";

  // Touch the guard file.
  command2 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command2.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> reap = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to finish.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  Try<string> read = os::read(transmissionTime);
  ASSERT_SOME(read);

  // Get the real elapsed time from `time` output. Sample output:
  // real 12.37
  // user 0.00
  // sys 0.00
  vector<string> lines = strings::split(strings::trim(read.get()), "\n");
  ASSERT_EQ(3u, lines.size());

  vector<string> split = strings::split(lines[0], " ");
  ASSERT_EQ(2u, split.size());

  Try<float> time = numify<float>(split[1]);
  ASSERT_SOME(time);
  ASSERT_GT(time.get(), (size.bytes() / rate.bytes()));

  // Make sure the nc server exits normally.
  Future<Option<int>> status = s->status();
  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where PortMappingIsolator uses a very small
// ingress rate limit.
TEST_F(PortMappingIsolatorTest, ROOT_NC_SmallIngressLimit)
{
  const Bytes rate = 2000;
  const Bytes size = 20480;

  flags.ingress_rate_limit_per_container = rate;
  flags.minimum_ingress_rate_limit = 0;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo = isolator.get()->prepare(
      containerId, containerConfig);
  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream cmd1;
  cmd1 << "touch " << container1Ready << " && ";
  cmd1 << "nc -l localhost " << validPort << " > /dev/null";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      cmd1.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> reap = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to finish.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  Result<htb::cls::Config> config = htb::cls::getConfig(
      slave::PORT_MAPPING_VETH_PREFIX() + stringify(pid.get()),
      routing::Handle(routing::Handle(1, 0), 1));
  ASSERT_SOME(config);
  EXPECT_EQ(rate, config->rate);

  const string data(size.bytes(), 'a');

  ostringstream cmd2;
  cmd2 << "echo " << data << " | nc localhost " << validPort;

  Stopwatch stopwatch;
  stopwatch.start();
  ASSERT_SOME(os::shell(cmd2.str()));
  Duration time = stopwatch.elapsed();

  // Allow the time to deviate up to 1sec here to compensate for burstness.
  Duration expectedTime = Seconds(size.bytes() / rate.bytes() - 1);
  ASSERT_GE(time, expectedTime);

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


TEST_F(PortMappingIsolatorTest, ROOT_ScaleEgressWithCPU)
{
  flags.egress_rate_limit_per_container = None();

  const Bytes egressRatePerCpu = 1000;
  flags.egress_rate_per_cpu = stringify(egressRatePerCpu);

  const Bytes minRate = 2000;
  flags.minimum_egress_rate_limit = minRate;

  const Bytes maxRate = 4000;
  flags.maximum_egress_rate_limit = maxRate;

  // CPU low enough for scaled network egress to be increased to
  // min limit: 1 * 1000 < 2000 ==> egress is 2000.
  Try<Resources> lowCpu = Resources::parse("cpus:1;mem:1024;disk:1024");
  ASSERT_SOME(lowCpu);

  // CPU sufficient to be in linear scaling region, greater than min
  // and less than max: 2000 < 3.1 * 1000 < 4000.
  Try<Resources> linearCpu = Resources::parse("cpus:3.1;mem:1024;disk:1024");
  ASSERT_SOME(linearCpu);

  // CPU high enough for scaled network egress to be reduced to the
  // max limit: 5 * 1000 > 4000.
  Try<Resources> highCpu = Resources::parse("cpus:5;mem:1024;disk:1024");
  ASSERT_SOME(highCpu);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(lowCpu.get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(containerId1, containerConfig1);
  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      "touch " + container1Ready + " && sleep 1000",
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Signal forked child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for command to start to ensure all pre-exec scripts have
  // executed.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  Result<htb::cls::Config> config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(minRate, config->rate);

  // Increase CPU to get to linear scaling.
  Future<Nothing> update = isolator.get()->update(
      containerId1,
      linearCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(
      egressRatePerCpu.bytes() * floor(linearCpu.get().cpus().get()),
      config->rate);

  // Increase CPU further to hit maximum limit.
  update = isolator.get()->update(
      containerId1,
      highCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(maxRate, config->rate);

  // Kill the container
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete launcher.get();
  delete isolator.get();
}


// Test that egress rate limit scaling works correctly with high real-world
// speeds.
TEST_F(PortMappingIsolatorTest, ROOT_ScaleEgressWithCPULarge)
{
  flags.egress_rate_limit_per_container = None();
  flags.egress_rate_per_cpu = "auto";

  // Change available CPUs to be 64.
  vector<string> resources = strings::split(flags.resources.get(), ";");
  std::replace_if(
      resources.begin(),
      resources.end(),
      [](const string& s) {return strings::startsWith(s, "cpus:");},
      "cpus:64");
  flags.resources = strings::join(";", resources);

  const Bytes linkSpeed = 12500000000; // 100 Gbit/s
  flags.network_link_speed = linkSpeed;

  const Bytes minRate = 625000000; // 5 Gbit/s
  flags.minimum_egress_rate_limit = minRate;

  const Bytes maxRate = 11250000000; // 90 Gbit/s
  flags.maximum_egress_rate_limit = maxRate;

  const Bytes ratePerCpu = linkSpeed / 64;

  // CPU high enough to be in linear scaling region and to trigger uint32_t
  // overflow of scaled rate represented as bit/s: 30 * 12500000000 / 64 =
  // 5859375000 or 46875000000 bits/s.
  Try<Resources> linearCpu = Resources::parse("cpus:30;mem:1024;disk:1024");
  ASSERT_SOME(linearCpu);

  // CPU low enough for scaled network egress to be increased to the min limit:
  // 1 * 12500000000 / 64 = 195312500 B/s.
  Try<Resources> lowCpu = Resources::parse("cpus:1;mem:1024;disk:1024");
  ASSERT_SOME(lowCpu);

  // CPU high enough for scaled network egress to be reduced to the max limit:
  // 60 * 12500000000 / 64 = 11718750000 B/s.
  Try<Resources> highCpu = Resources::parse("cpus:60;mem:1024;disk:1024");
  ASSERT_SOME(highCpu);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(linearCpu.get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig1.mutable_resources()->CopyFrom(executorInfo.resources());

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(containerId1, containerConfig1);
  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      "touch " + container1Ready + " && sleep 1000",
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Signal forked child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for command to start to ensure all pre-exec scripts have
  // executed.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  Result<htb::cls::Config> config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(ratePerCpu * floor(linearCpu->cpus().get()), config->rate);

  // Reduce CPU to get to hit the min limit.
  Future<Nothing> update = isolator.get()->update(containerId1, lowCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(minRate, config->rate);

  // Increase CPU back to the linear limit.
  update = isolator.get()->update(containerId1, linearCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(ratePerCpu * floor(linearCpu->cpus().get()), config->rate);

  // Increase CPU to hit the max limit.
  update = isolator.get()->update(containerId1, highCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(maxRate, config->rate);

  // Kill the container
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete launcher.get();
  delete isolator.get();
}


TEST_F(PortMappingIsolatorTest, ROOT_ScaleEgressWithCPUAutoConfig)
{
  flags.egress_rate_limit_per_container = None();
  flags.network_link_speed = Bytes(10000);

  // Change available CPUs to be 10.
  vector<string> resources = strings::split(flags.resources.get(), ";");
  std::replace_if(
      resources.begin(),
      resources.end(),
      [](const string& s) {return strings::startsWith(s, "cpus:");},
      "cpus:10");
  flags.resources = strings::join(";", resources);

  // Egress rate limit per CPU should be 10000 / 10 = 1000.
  const Bytes egressRatePerCpu = Bytes(1000);
  flags.egress_rate_per_cpu = "auto";

  const Bytes minRate = 2000;
  flags.minimum_egress_rate_limit = minRate;

  const Bytes maxRate = 4000;
  flags.maximum_egress_rate_limit = maxRate;

  // CPU low enough for scaled network ingress to be increased to min limit:
  // 1 * 1000 < 2000 ==> ingress is 2000.
  Try<Resources> lowCpu = Resources::parse("cpus:1;mem:1024;disk:1024");
  ASSERT_SOME(lowCpu);

  // CPU sufficient to be in linear scaling region, greater than min and less
  // than max: 2000 < 3.1 * 1000 < 4000.
  Try<Resources> linearCpu = Resources::parse("cpus:3.1;mem:1024;disk:1024");
  ASSERT_SOME(linearCpu);

  // CPU high enough for scaled network ingress to be reduced to the max limit:
  // 5 * 1000 > 4000.
  Try<Resources> highCpu = Resources::parse("cpus:5;mem:1024;disk:1024");
  ASSERT_SOME(highCpu);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // The isolator should report the calculated per-CPU limit as a metric.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(egressRatePerCpu.bytes(),
            metrics.values["port_mapping/per_cpu_egress_rate_limit"]);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(lowCpu.get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(containerId1, containerConfig1);
  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      "touch " + container1Ready + " && sleep 1000",
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Signal forked child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for command to start to ensure all pre-exec scripts have
  // executed.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  // The container should start with minimum limit.
  Result<htb::cls::Config> config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(minRate, config->rate);

  // Increase CPU to get to linear scaling.
  Future<Nothing> update = isolator.get()->update(
      containerId1, linearCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(egressRatePerCpu.bytes() * floor(linearCpu->cpus().get()),
            config->rate);

  // Increase CPU further to hit maximum limit.
  update = isolator.get()->update(containerId1, highCpu.get());
  AWAIT_READY(update);

  config = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(config);
  ASSERT_EQ(maxRate, config->rate);

  // Kill the container
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete launcher.get();
  delete isolator.get();
}


TEST_F(PortMappingIsolatorTest, ROOT_ScaleIngressWithCPU)
{
  flags.ingress_rate_limit_per_container = None();

  const Bytes ingressRatePerCpu = 1000;
  flags.ingress_rate_per_cpu = stringify(ingressRatePerCpu);

  const Bytes minRate = 2000;
  flags.minimum_ingress_rate_limit = minRate;

  const Bytes maxRate = 4000;
  flags.maximum_ingress_rate_limit = maxRate;

  // CPU low enough for scaled network ingress to be increased to min
  // limit: 1 * 1000 < 2000 ==> ingress is 2000.
  Try<Resources> lowCpu = Resources::parse("cpus:1;mem:1024;disk:1024");
  ASSERT_SOME(lowCpu);

  // CPU sufficient to be in linear scaling region, greater than min
  // and less than max: 2000 < 3.1 * 1000 < 4000.
  Try<Resources> linearCpu = Resources::parse("cpus:3.1;mem:1024;disk:1024");
  ASSERT_SOME(linearCpu);

  // CPU high enough for scaled network ingress to be reduced to the
  // max limit: 5 * 1000 > 4000.
  Try<Resources> highCpu = Resources::parse("cpus:5;mem:1024;disk:1024");
  ASSERT_SOME(highCpu);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(lowCpu.get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(containerId1, containerConfig1);
  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      "touch " + container1Ready + " && sleep 1000",
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Signal forked child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for command to start to ensure all pre-exec scripts have
  // executed.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  const string veth = slave::PORT_MAPPING_VETH_PREFIX() + stringify(pid.get());
  const routing::Handle cls(routing::Handle(1, 0), 1);

  Result<htb::cls::Config> config = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(config);
  ASSERT_EQ(minRate, config->rate);

  // Increase CPU to get to linear scaling.
  Future<Nothing> update = isolator.get()->update(
      containerId1,
      linearCpu.get());
  AWAIT_READY(update);

  config = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(config);
  ASSERT_EQ(
      ingressRatePerCpu.bytes() * floor(linearCpu.get().cpus().get()),
      config->rate);

  // Increase CPU further to hit maximum limit.
  update = isolator.get()->update(
      containerId1,
      highCpu.get());
  AWAIT_READY(update);

  config = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(config);
  ASSERT_EQ(maxRate, config->rate);

  // Kill the container
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete launcher.get();
  delete isolator.get();
}


TEST_F(PortMappingIsolatorTest, ROOT_ScaleIngressWithCPUAutoConfig)
{
  flags.ingress_rate_limit_per_container = None();
  flags.network_link_speed = Bytes(10000);

  // Change available CPUs to be 10.
  vector<string> resources = strings::split(flags.resources.get(), ";");
  std::replace_if(
      resources.begin(),
      resources.end(),
      [](const string& s) {return strings::startsWith(s, "cpus:");},
      "cpus:10");

  flags.resources = strings::join(";", resources);

  // Ingress rate limit per CPU should be 10000 / 10 = 1000.
  const Bytes ingressRatePerCpu = Bytes(1000);
  flags.ingress_rate_per_cpu = "auto";

  const Bytes minRate = 2000;
  flags.minimum_ingress_rate_limit = minRate;

  const Bytes maxRate = 4000;
  flags.maximum_ingress_rate_limit = maxRate;

  // CPU low enough for scaled network ingress to be increased to min
  // limit: 1 * 1000 < 2000 ==> ingress is 2000.
  Try<Resources> lowCpu = Resources::parse("cpus:1;mem:1024;disk:1024");
  ASSERT_SOME(lowCpu);

  // CPU sufficient to be in linear scaling region, greater than min
  // and less than max: 2000 < 3.1 * 1000 < 4000.
  Try<Resources> linearCpu = Resources::parse("cpus:3.1;mem:1024;disk:1024");
  ASSERT_SOME(linearCpu);

  // CPU high enough for scaled network ingress to be reduced to the
  // max limit: 5 * 1000 > 4000.
  Try<Resources> highCpu = Resources::parse("cpus:5;mem:1024;disk:1024");
  ASSERT_SOME(highCpu);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // The isolator should report the calculated per-CPU limit as a metric.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(ingressRatePerCpu.bytes(),
            metrics.values["port_mapping/per_cpu_ingress_rate_limit"]);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(lowCpu.get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator.get()->prepare(containerId1, containerConfig1);
  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      "touch " + container1Ready + " && sleep 1000",
      launchInfo1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Signal forked child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for command to start to ensure all pre-exec scripts have
  // executed.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  const string veth = slave::PORT_MAPPING_VETH_PREFIX() + stringify(pid.get());
  const routing::Handle cls(routing::Handle(1, 0), 1);

  Result<htb::cls::Config> config = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(config);
  ASSERT_EQ(minRate, config->rate);

  // Increase CPU to get to linear scaling.
  Future<Nothing> update = isolator.get()->update(
      containerId1,
      linearCpu.get());
  AWAIT_READY(update);

  config = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(config);
  ASSERT_EQ(
      ingressRatePerCpu.bytes() * floor(linearCpu.get().cpus().get()),
      config->rate);

  // Increase CPU further to hit maximum limit.
  update = isolator.get()->update(
      containerId1,
      highCpu.get());
  AWAIT_READY(update);

  config = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(config);
  ASSERT_EQ(maxRate, config->rate);

  // Kill the container
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete launcher.get();
  delete isolator.get();
}


TEST_F(PortMappingIsolatorTest, ROOT_Upgrade)
{
  const Bytes rate = Bytes(1000);

  flags.minimum_egress_rate_limit = 0;
  flags.egress_rate_limit_per_container = None();
  flags.minimum_ingress_rate_limit = 0;
  flags.ingress_rate_limit_per_container = None();
  flags.ingress_isolate_existing_containers = false;

  Try<Resources> resources = Resources::parse("cpus:1;mem:128;disk:1024");
  ASSERT_SOME(resources);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(resources.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(containerId, containerConfig);
  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      "touch " + container1Ready + " && sleep 1000",
      launchInfo.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Signal forked child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for command to start to ensure all pre-exec scripts have
  // executed.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  const string veth = slave::PORT_MAPPING_VETH_PREFIX() + stringify(pid.get());
  const routing::Handle cls(routing::Handle(1, 0), 1);

  Result<htb::cls::Config> egressConfig =
    recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_NONE(egressConfig);

  Result<htb::cls::Config> ingressConfig = htb::cls::getConfig(veth, cls);
  ASSERT_NONE(ingressConfig);

  // Turn rate limiting on.
  flags.egress_rate_limit_per_container = rate;
  flags.ingress_rate_limit_per_container = rate;

  // Recreate the isolator with the new flags.
  delete isolator.get();
  isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  ContainerState containerState;
  containerState.mutable_container_id()->CopyFrom(containerId);
  containerState.set_pid(pid.get());

  // Recover and rightsize the container as the agent does upon
  // executor re-registration.
  AWAIT_READY(isolator.get()->recover({containerState}, {}));
  AWAIT_READY(isolator.get()->update(containerId, resources.get()));

  egressConfig = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(egressConfig);
  ASSERT_EQ(rate, egressConfig->rate);

  // Ingress isolation should not be turned on because we didn't allow
  // upgrading existing containers.
  ingressConfig = htb::cls::getConfig(veth, cls);
  ASSERT_NONE(ingressConfig);

  // Enable turning on ingress isolation for existing containers.
  flags.ingress_isolate_existing_containers = true;

  // Recreate the isolator with the new flags.
  delete isolator.get();
  isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);
  AWAIT_READY(isolator.get()->recover({containerState}, {}));
  AWAIT_READY(isolator.get()->update(containerId, resources.get()));

  egressConfig = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_SOME(egressConfig);
  ASSERT_EQ(rate, egressConfig->rate);

  ingressConfig = htb::cls::getConfig(veth, cls);
  ASSERT_SOME(ingressConfig);
  ASSERT_EQ(rate, ingressConfig->rate);

  // Turn rate limiting off.
  flags.egress_rate_limit_per_container = None();
  flags.ingress_rate_limit_per_container = None();

  // Recreate the isolator with the new flags.
  delete isolator.get();
  isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);
  AWAIT_READY(isolator.get()->recover({containerState}, {}));
  AWAIT_READY(isolator.get()->update(containerId, resources.get()));

  egressConfig = recoverHTBConfig(pid.get(), eth0, flags);
  ASSERT_NONE(egressConfig);

  ingressConfig = htb::cls::getConfig(veth, cls);
  ASSERT_NONE(ingressConfig);

  // Kill the container.
  AWAIT_READY(launcher.get()->destroy(containerId));
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete launcher.get();
  delete isolator.get();
}


bool HasTCPSocketsCount(const ResourceStatistics& statistics)
{
  return statistics.has_net_tcp_active_connections() &&
    statistics.has_net_tcp_time_wait_connections();
}


bool HasTCPSocketsRTT(const ResourceStatistics& statistics)
{
  // We either have all of the following metrics or we have nothing.
  if (statistics.has_net_tcp_rtt_microsecs_p50() &&
      statistics.has_net_tcp_rtt_microsecs_p90() &&
      statistics.has_net_tcp_rtt_microsecs_p95() &&
      statistics.has_net_tcp_rtt_microsecs_p99()) {
    return true;
  } else {
    return false;
  }
}


bool HasTCPRetransSegs(const ResourceStatistics& statistics)
{
  return statistics.has_net_snmp_statistics() &&
         statistics.net_snmp_statistics().has_tcp_stats() &&
         statistics.net_snmp_statistics().tcp_stats().has_retranssegs();
}


Try<Interval<uint16_t>> getEphemeralPortRange(pid_t pid)
{
  Try<string> out = os::shell(
      "ip netns exec " + stringify(pid) +
      " cat /proc/sys/net/ipv4/ip_local_port_range");
  if (out.isError()) {
    return Error("Failed to read ip_local_port_range: " + out.error());
  }

  vector<string> ports = strings::split(strings::trim(out.get()), "\t");
  if (ports.size() != 2) {
    return Error("Unexpected ip_local_port_range format: " + out.get());
  }

  Try<uint16_t> begin = numify<uint16_t>(ports[0]);
  if (begin.isError()) {
    return Error("Failed to parse begin of ip_local_port_range: " + ports[0]);
  }

  Try<uint16_t> end = numify<uint16_t>(ports[1]);
  if (end.isError()) {
    return Error("Failed to parse end of ip_local_port_range: " + ports[1]);
  }

  return (Bound<uint16_t>::closed(begin.get()),
          Bound<uint16_t>::closed(end.get()));
}


// Test that RTT can be returned properly from usage(). This test is
// very similar to SmallEgressLimitTest in its setup.
TEST_F(PortMappingIsolatorTest, ROOT_NC_PortMappingStatistics)
{
  // To-be-tested egress rate limit, in Bytes/s.
  const Bytes rate = 2000;
  // Size of the data to send, in Bytes.
  const Bytes size = 20480;

  // Use a very small egress limit.
  flags.egress_rate_limit_per_container = rate;
  flags.minimum_egress_rate_limit = 0;
  flags.network_enable_socket_statistics_summary = true;
  flags.network_enable_socket_statistics_details = true;
  flags.network_enable_snmp_statistics = true;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  // Open an nc server on the host side. Note that 'invalidPort' is
  // in neither 'ports' nor 'ephemeral_ports', which makes it a good
  // port to use on the host. We use this host's public IP because
  // connections to the localhost IP are filtered out when retrieving
  // the RTT information inside containers.
  ostringstream command1;
  command1 << "nc -l " << hostIP << " " << invalidPort << " > " << os::DEV_NULL;
  Try<Subprocess> s = subprocess(command1.str().c_str());
  ASSERT_SOME(s);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo =
    isolator.get()->prepare(
        containerId,
        containerConfig);

  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  // Fill 'size' bytes of data. The actual content does not matter.
  string data(size.bytes(), 'a');

  ostringstream command2;
  const string transmissionTime = path::join(os::getcwd(), "transmission_time");

  command2 << "echo 'Sending " << size.bytes()
           << " bytes of data under egress rate limit " << rate.bytes()
           << "Bytes/s...';";

  command2 << "{ time -p echo " << data << " | nc " << hostIP << " "
           << invalidPort << " ; } 2> " << transmissionTime << " && ";

  // Touch the guard file.
  command2 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command2.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> reap = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Test that RTT can be returned while transmission is going. It is
  // possible that the first few statistics returned don't have a RTT
  // value because it takes a few round-trips to actually establish a
  // tcp connection and start sending data. Nevertheless, we should
  // see a meaningful result well within seconds.
  Duration waited = Duration::zero();
  do {
    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);

    // Do an end-to-end test by calling `usage`.
    Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
    AWAIT_READY(usage);

    if (usage->has_net_tcp_rtt_microsecs_p50() &&
        usage->has_net_tcp_active_connections()) {
      EXPECT_GT(usage->net_tcp_active_connections(), 0);

      // Test that requested number of ephemeral ports is reported.
      ASSERT_TRUE(usage->has_net_ephemeral_ports());
      const Value::Range& ephemeralPorts = usage->net_ephemeral_ports();
      EXPECT_EQ(flags.ephemeral_ports_per_container,
                ephemeralPorts.end() - ephemeralPorts.begin() + 1u);

      // Test that reported ports range is same as the one inside the
      // container.
      Try<Interval<uint16_t>> ports = getEphemeralPortRange(pid.get());
      ASSERT_SOME(ports);
      EXPECT_EQ(ports->lower(), ephemeralPorts.begin());
      EXPECT_EQ(ports->upper() - 1u, ephemeralPorts.end());

      break;
    }
  } while (waited < Seconds(5));
  ASSERT_LT(waited, Seconds(5));

  // While the connection is still active, try out different flag
  // combinations.
  Result<ResourceStatistics> statistics =
      statisticsHelper(pid.get(), true, true, true);
  ASSERT_SOME(statistics);
  EXPECT_TRUE(HasTCPSocketsCount(statistics.get()));
  EXPECT_TRUE(HasTCPSocketsRTT(statistics.get()));
  EXPECT_TRUE(HasTCPRetransSegs(statistics.get()));

  statistics = statisticsHelper(pid.get(), true, false, false);
  ASSERT_SOME(statistics);
  EXPECT_TRUE(HasTCPSocketsCount(statistics.get()));
  EXPECT_FALSE(HasTCPSocketsRTT(statistics.get()));
  EXPECT_FALSE(HasTCPRetransSegs(statistics.get()));

  statistics = statisticsHelper(pid.get(), false, true, true);
  ASSERT_SOME(statistics);
  EXPECT_FALSE(HasTCPSocketsCount(statistics.get()));
  EXPECT_TRUE(HasTCPSocketsRTT(statistics.get()));
  EXPECT_TRUE(HasTCPRetransSegs(statistics.get()));

  statistics = statisticsHelper(pid.get(), false, false, false);
  ASSERT_SOME(statistics);
  EXPECT_FALSE(HasTCPSocketsCount(statistics.get()));
  EXPECT_FALSE(HasTCPSocketsRTT(statistics.get()));
  EXPECT_FALSE(HasTCPRetransSegs(statistics.get()));

  // Wait for the command to finish.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  // Make sure the nc server exits normally.
  Future<Option<int>> status = s->status();
  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Verify that rate statistics can be returned properly from
// 'usage()'. This test is very similar to SmallIngressLimitTest in
// setup.
TEST_F(PortMappingIsolatorTest, ROOT_NC_PortMappingRateStatistics)
{
  const Bytes rate = 2000;
  const Bytes size = 20480;

  flags.ingress_rate_limit_per_container = rate;
  flags.minimum_ingress_rate_limit = 0;
  flags.network_enable_rate_statistics = true;
  flags.network_rate_statistics_window = Seconds(5);
  flags.network_rate_statistics_interval = Milliseconds(50);

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  ASSERT_SOME(launcher);

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(dir.get());

  Future<Option<ContainerLaunchInfo>> launchInfo = isolator.get()->prepare(
      containerId, containerConfig);
  AWAIT_READY(launchInfo);
  ASSERT_SOME(launchInfo.get());
  ASSERT_EQ(1, launchInfo.get()->pre_exec_commands().size());

  ostringstream cmd1;
  cmd1 << "touch " << container1Ready << " && ";
  cmd1 << "nc -l -k localhost " << validPort << " > /dev/null";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      cmd1.str(),
      launchInfo.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int>> reap = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to finish.
  ASSERT_TRUE(waitForFileCreation(container1Ready));

  const string data(size.bytes(), 'a');

  ostringstream cmd2;
  cmd2 << "echo " << data << " | nc localhost " << validPort;

  Stopwatch stopwatch;
  stopwatch.start();
  ASSERT_SOME(os::shell(cmd2.str()));
  Duration time = stopwatch.elapsed();

  // Allow the time to deviate up to 1sec here to compensate for burstness.
  Duration expectedTime = Seconds(size.bytes() / rate.bytes() - 1);
  ASSERT_GE(time, expectedTime);

  // Number of samples that should fit into the window.
  const size_t samples = flags.network_rate_statistics_window->secs() /
    flags.network_rate_statistics_interval->secs();

  // Verify that TX and RX rates have been returned with resource
  // statistics. It's hard to verify actual values here because of
  // burstness.
  Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
  AWAIT_READY(usage);
  EXPECT_TRUE(usage->has_net_rate_statistics());

  const ResourceStatistics::RateStatistics& rates =
    usage->net_rate_statistics();
  EXPECT_TRUE(rates.has_tx_rate());
  EXPECT_TRUE(rates.tx_rate().has_p90());
  EXPECT_TRUE(rates.tx_rate().has_samples());
  EXPECT_GE(samples, rates.tx_rate().samples());
  EXPECT_TRUE(rates.has_tx_packet_rate());
  EXPECT_TRUE(rates.tx_packet_rate().has_p90());
  EXPECT_TRUE(rates.has_tx_drop_rate());
  EXPECT_TRUE(rates.tx_drop_rate().has_p90());
  EXPECT_TRUE(rates.has_tx_error_rate());
  EXPECT_TRUE(rates.tx_error_rate().has_p90());
  EXPECT_TRUE(rates.has_rx_rate());
  EXPECT_TRUE(rates.rx_rate().has_p90());
  EXPECT_TRUE(rates.has_rx_packet_rate());
  EXPECT_TRUE(rates.rx_packet_rate().has_p90());
  EXPECT_TRUE(rates.has_rx_drop_rate());
  EXPECT_TRUE(rates.rx_drop_rate().has_p90());
  EXPECT_TRUE(rates.has_rx_error_rate());
  EXPECT_TRUE(rates.rx_error_rate().has_p90());

  EXPECT_TRUE(rates.has_sampling_window_secs());
  EXPECT_EQ(
      flags.network_rate_statistics_window->secs(),
      rates.sampling_window_secs());
  EXPECT_TRUE(rates.has_sampling_interval_secs());
  EXPECT_EQ(
      flags.network_rate_statistics_interval->secs(),
      rates.sampling_interval_secs());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Verify that PercentileRatesCollector calculates rates correctly.
TEST(RatesCollectorTest, PercentileRatesCollector)
{
  Clock::pause();
  const Duration window = Seconds(10);
  const Duration interval = Seconds(1);
  const Time now = Clock::now();

  PercentileRatesCollector collector(0, window, interval);

  // No samples.
  EXPECT_NONE(collector.txRate());
  EXPECT_NONE(collector.txPacketRate());
  EXPECT_NONE(collector.txDropRate());
  EXPECT_NONE(collector.txErrorRate());
  EXPECT_NONE(collector.rxRate());
  EXPECT_NONE(collector.rxPacketRate());
  EXPECT_NONE(collector.rxDropRate());
  EXPECT_NONE(collector.rxErrorRate());

  const auto createSample = [](
      uint64_t txBytes,
      uint64_t txPackets,
      uint64_t txDropped,
      uint64_t txErrors,
      uint64_t rxBytes,
      uint64_t rxPackets,
      uint64_t rxDropped,
      uint64_t rxErrors) -> hashmap<string, uint64_t> {
    return {{"tx_bytes", txBytes},
            {"tx_packets", txPackets},
            {"tx_dropped", txDropped},
            {"tx_errors", txErrors},
            {"rx_bytes", rxBytes},
            {"rx_packets", rxPackets},
            {"rx_dropped", rxDropped},
            {"rx_errors", rxErrors}};
  };

  // Simulate ingress traffic burst at 100 B/s for the first 2 sec and
  // steady 50 B/s rate for 3 sec after that. Egress traffic rate was
  // 10 B/s for the first 2 sec and 0 for the rest of the time.
  collector.sample(now, createSample(0, 0, 0, 0, 0, 0, 0, 0));
  collector.sample(now + Seconds(1), createSample(100, 10, 5, 1, 10, 1, 1, 1));
  collector.sample(now + Seconds(2), createSample(200, 20, 10, 2, 20, 2, 2, 2));
  collector.sample(now + Seconds(3), createSample(250, 25, 11, 3, 20, 2, 2, 2));
  collector.sample(now + Seconds(4), createSample(300, 30, 12, 4, 20, 2, 2, 2));
  collector.sample(now + Seconds(5), createSample(350, 35, 13, 5, 20, 2, 2, 2));

  Option<Statistics<uint64_t>> rxRate = collector.rxRate();
  ASSERT_SOME(rxRate);
  EXPECT_EQ(5u, rxRate->count);   // Number of statistics samples.
  EXPECT_EQ(100u, rxRate->max);   // Max seen byte rate.
  EXPECT_EQ(50u, rxRate->min);    // Min seen byte rate.
  EXPECT_EQ(100u, rxRate->p90);   // p90 is 4.5th sample here.
  EXPECT_EQ(50u, rxRate->p50);    // p50 is 2.5th sample here.

  Option<Statistics<uint64_t>> rxPacketRate = collector.rxPacketRate();
  ASSERT_SOME(rxPacketRate);
  EXPECT_EQ(5u, rxPacketRate->count);
  EXPECT_EQ(10u, rxPacketRate->max);
  EXPECT_EQ(5u, rxPacketRate->min);
  EXPECT_EQ(10u, rxPacketRate->p90);
  EXPECT_EQ(5u, rxPacketRate->p50);

  Option<Statistics<uint64_t>> rxDropRate = collector.rxDropRate();
  ASSERT_SOME(rxDropRate);
  EXPECT_EQ(5u, rxDropRate->count);
  EXPECT_EQ(5u, rxDropRate->max);
  EXPECT_EQ(1u, rxDropRate->min);
  EXPECT_EQ(5u, rxDropRate->p90);
  EXPECT_EQ(1u, rxDropRate->p50);

  // RX error rate is constantly 1 here.
  Option<Statistics<uint64_t>> rxErrorRate = collector.rxErrorRate();
  ASSERT_SOME(rxErrorRate);
  EXPECT_EQ(5u, rxErrorRate->count);
  EXPECT_EQ(1u, rxErrorRate->max);
  EXPECT_EQ(1u, rxErrorRate->min);
  EXPECT_EQ(1u, rxErrorRate->p90);
  EXPECT_EQ(1u, rxErrorRate->p50);

  Option<Statistics<uint64_t>> txRate = collector.txRate();
  ASSERT_SOME(txRate);
  EXPECT_EQ(5u, txRate->count);
  EXPECT_EQ(10u, txRate->max);
  EXPECT_EQ(0u, txRate->min);
  EXPECT_EQ(10u, txRate->p90);
  EXPECT_EQ(0u, txRate->p50);

  Option<Statistics<uint64_t>> txPacketRate = collector.txPacketRate();
  ASSERT_SOME(txPacketRate);
  EXPECT_EQ(5u, txPacketRate->count);
  EXPECT_EQ(1u, txPacketRate->max);
  EXPECT_EQ(0u, txPacketRate->min);
  EXPECT_EQ(1u, txPacketRate->p90);
  EXPECT_EQ(0u, txPacketRate->p50);

  Option<Statistics<uint64_t>> txDropRate = collector.txDropRate();
  ASSERT_SOME(txDropRate);
  EXPECT_EQ(5u, txDropRate->count);
  EXPECT_EQ(1u, txDropRate->max);
  EXPECT_EQ(0u, txDropRate->min);
  EXPECT_EQ(1u, txDropRate->p90);
  EXPECT_EQ(0u, txDropRate->p50);

  Option<Statistics<uint64_t>> txErrorRate = collector.txErrorRate();
  ASSERT_SOME(txErrorRate);
  EXPECT_EQ(5u, txErrorRate->count);
  EXPECT_EQ(1u, txErrorRate->max);
  EXPECT_EQ(0u, txErrorRate->min);
  EXPECT_EQ(1u, txErrorRate->p90);
  EXPECT_EQ(0u, txErrorRate->p50);

  Clock::resume();
}


static uint16_t roundUpToPow2(uint16_t x)
{
  uint16_t r = 1 << static_cast<uint16_t>(std::log2(x));
  return x == r ? x : (r << 1);
}


// This test verifies that the isolator properly cleans up the
// container that was not isolated, and doesn't leak ephemeral ports.
TEST_F(PortMappingIsolatorTest, ROOT_CleanupNotIsolated)
{
  Try<Resources> resources = Resources::parse(flags.resources.get());
  ASSERT_SOME(resources);
  Try<IntervalSet<uint16_t>> ephemeralPorts =
    rangesToIntervalSet<uint16_t>(resources->ephemeral_ports().get());
  ASSERT_SOME(ephemeralPorts);

  // Increase the number of ephemeral ports per container so that we
  // won't be able to launch a second container unless ports used by
  // the first one are deallocated.
  flags.ephemeral_ports_per_container =
    roundUpToPow2(ephemeralPorts->size() / 2 + 1);

  Try<Isolator*> _isolator = PortMappingIsolatorProcess::create(flags);
  ASSERT_SOME(_isolator);
  Owned<Isolator> isolator(_isolator.get());

  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value(id::UUID::random().toString());

  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  ContainerConfig containerConfig1;
  containerConfig1.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig1.set_directory(dir1.get());

  Future<Option<ContainerLaunchInfo>> launchInfo1 =
    isolator->prepare(containerId1, containerConfig1);
  AWAIT_READY(launchInfo1);
  ASSERT_SOME(launchInfo1.get());
  ASSERT_EQ(1, launchInfo1.get()->pre_exec_commands().size());

  // Simulate container destruction during preparation and clean up
  // not isolated container.
  AWAIT_READY(isolator->cleanup(containerId1));

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  ContainerID containerId2;
  containerId2.set_value(id::UUID::random().toString());

  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  ContainerConfig containerConfig2;
  containerConfig2.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig2.set_directory(dir2.get());

  Future<Option<ContainerLaunchInfo>> launchInfo2 =
    isolator->prepare(containerId2, containerConfig2);
  AWAIT_READY(launchInfo2);
  ASSERT_SOME(launchInfo2.get());
  ASSERT_EQ(1, launchInfo2.get()->pre_exec_commands().size());

  AWAIT_READY(isolator->cleanup(containerId2));
}


class PortMappingMesosTest : public ContainerizerTest<MesosContainerizer>
{
public:
  virtual void SetUp()
  {
    ContainerizerTest<MesosContainerizer>::SetUp();

    // Guess the name of the public interface.
    Result<string> _eth0 = link::eth0();
    ASSERT_SOME(_eth0) << "Failed to guess the name of the public interface";

    eth0 = _eth0.get();

    LOG(INFO) << "Using " << eth0 << " as the public interface";

    // Guess the name of the loopback interface.
    Result<string> _lo = link::lo();
    ASSERT_SOME(_lo) << "Failed to guess the name of the loopback interface";

    lo = _lo.get();

    LOG(INFO) << "Using " << lo << " as the loopback interface";

    cleanup(eth0, lo);
  }

  virtual void TearDown()
  {
    cleanup(eth0, lo);

    ContainerizerTest<MesosContainerizer>::TearDown();
  }

  // Name of the host eth0 and lo.
  string eth0;
  string lo;
};


// Test the scenario where the network isolator is asked to recover
// both types of containers: containers that were previously managed
// by network isolator, and containers that weren't.
TEST_F(PortMappingMesosTest, ROOT_CGROUPS_RecoverMixedContainers)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start the first slave without the network isolator.
  slave::Flags slaveFlags = CreateSlaveFlags();

  // NOTE: This is to make sure that we use the linux launcher which
  // is consistent with the launchers we use for other containerizers
  // we create in this test. Also, this will bypass MESOS-2554.
  slaveFlags.isolation = "cgroups/cpu,cgroups/mem";

  Fetcher fetcher(slaveFlags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(slaveFlags, true, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      slaveFlags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Filters filters;
  filters.set_refuse_seconds(0);

  // NOTE: We set filter explicitly here so that the resources will
  // not be filtered for 5 seconds (the default).
  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(DeclineOffers(filters));      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  // Start a long running task without using the network isolator.
  TaskInfo task1 = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:1;mem:512").get(),
      "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);

  slave.get()->terminate();

  Future<Nothing> _recover1 = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage1 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(DeclineOffers(filters));      // Ignore subsequent offers.

  // Restart the slave with the network isolator.
  slaveFlags.isolation += ",network/port_mapping";

  _containerizer = MesosContainerizer::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = StartSlave(detector.get(), containerizer.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover1);
  AWAIT_READY(slaveReregisteredMessage1);

  Clock::pause();

  Clock::settle(); // Make sure an allocation is scheduled.
  Clock::advance(masterFlags.allocation_interval);

  Clock::resume();

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  // Start a long running task using the network isolator.
  TaskInfo task2 = createTask(offer2, "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillRepeatedly(Return());

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers2.get()[0].id(), {task2});

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement2);

  slave.get()->terminate();

  Future<Nothing> _recover2 = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage2 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave with the network isolator. This is to verify
  // the slave recovery case where one task is running with the
  // network isolator and another task is running without it.
  _containerizer = MesosContainerizer::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = StartSlave(detector.get(), containerizer.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(_recover2);
  AWAIT_READY(slaveReregisteredMessage2);

  // Ensure that both containers (with and without network isolation)
  // were recovered.
  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(2u, containers->size());

  foreach (const ContainerID& containerId, containers.get()) {
    // Do some basic checks to make sure the network isolator can
    // handle mixed types of containers correctly.
    Future<ResourceStatistics> usage = containerizer.get()->usage(containerId);
    AWAIT_READY(usage);

    // TODO(chzhcn): Write a more thorough test for update.
  }

  driver.stop();
  driver.join();
}


// Test that all configurations (tc filters etc) is cleaned up for an
// orphaned container using the network isolator.
TEST_F(PortMappingMesosTest, ROOT_CGROUPS_CleanUpOrphan)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // NOTE: We add 'cgroups/cpu,cgroups/mem' to bypass MESOS-2554.
  flags.isolation = "cgroups/cpu,cgroups/mem,network/port_mapping";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Start a long running task using network islator.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  EXPECT_CALL(sched, statusUpdate(_, _))
    .Times(2);

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  // Wait for the ACKs to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);
  AWAIT_READY(_statusUpdateAcknowledgement2);

  Future<hashset<ContainerID>> containers = containerizer->containers();

  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *containers->begin();

  slave.get()->terminate();

  // Wipe the slave meta directory so that the slave will treat the
  // above running task as an orphan.
  ASSERT_SOME(os::rmdir(paths::getMetaRootDir(flags.work_dir)));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  containerizer.reset(_containerizer.get());

  // Restart the slave.
  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until slave recovery is complete.
  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);
  AWAIT_READY_FOR(_recover, Seconds(60));

  // Wait until the orphan containers are cleaned up.
  AWAIT_READY_FOR(containerizer.get()->wait(containerId), Seconds(60));
  AWAIT_READY(slaveRegisteredMessage);

  // Expect that qdiscs still exist on eth0 and lo but with no filters.
  Try<bool> hostEth0ExistsQdisc = ingress::exists(eth0);
  EXPECT_SOME_TRUE(hostEth0ExistsQdisc);

  Try<bool> hostLoExistsQdisc = ingress::exists(lo);
  EXPECT_SOME_TRUE(hostLoExistsQdisc);

  Result<vector<ip::Classifier>> classifiers =
    ip::classifiers(eth0, ingress::HANDLE);

  EXPECT_SOME(classifiers);
  EXPECT_TRUE(classifiers->empty());

  classifiers = ip::classifiers(lo, ingress::HANDLE);
  EXPECT_SOME(classifiers);
  EXPECT_TRUE(classifiers->empty());

  // Expect no 'veth' devices.
  Try<set<string>> links = net::links();
  ASSERT_SOME(links);
  foreach (const string& name, links.get()) {
    EXPECT_FALSE(strings::startsWith(name, slave::PORT_MAPPING_VETH_PREFIX()));
  }

  // Expect no files in bind mount directory.
  Try<list<string>> files = os::ls(slave::PORT_MAPPING_BIND_MOUNT_ROOT());
  ASSERT_SOME(files);
  EXPECT_TRUE(files->empty());

  driver.stop();
  driver.join();
}


// This test verifies the creation and destruction of the network
// namespace handle symlink. The symlink was introduced in 0.23.0.
TEST_F(PortMappingMesosTest, ROOT_NetworkNamespaceHandleSymlink)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/port_mapping";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Start a long running task using network islator.
  TaskInfo task = createTask(offers.get()[0], "sleep 1000");

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status0);
  EXPECT_EQ(task.task_id(), status0->task_id());
  EXPECT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1->task_id());
  EXPECT_EQ(TASK_RUNNING, status1->state());

  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  const string symlink = path::join(
      slave::PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT(),
      stringify(containerId));

  EXPECT_TRUE(os::exists(symlink));
  EXPECT_TRUE(os::stat::islink(symlink));

  Future<Option<ContainerTermination>> termination =
    containerizer->wait(containerId);

  driver.killTask(task.task_id());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2->task_id());
  EXPECT_EQ(TASK_KILLED, status2->state());

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  EXPECT_FALSE(os::exists(symlink));

  driver.stop();
  driver.join();
}


// This test verifies that the isolator is able to recover a mix of
// known and unknown orphans. This is used to capture the regression
// described in MESOS-2914.
TEST_F(PortMappingMesosTest, ROOT_CGROUPS_RecoverMixedKnownAndUnKnownOrphans)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/port_mapping";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task1 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:64").get(),
      "sleep 1000");

  TaskInfo task2 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:64").get(),
      "sleep 1000");

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task1, task2});

  // Only check the first and the last status, as the other two might
  // be interleaved between TASK_STARTING and TASK_RUNNING.
  AWAIT_READY(status1);
  ASSERT_EQ(TASK_STARTING, status1->state());

  AWAIT_READY(status4);
  ASSERT_EQ(TASK_RUNNING, status4->state());

  // Obtain the container IDs.
  Future<hashset<ContainerID>> containers = containerizer->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(2u, containers->size());

  auto iterator = containers->begin();
  const ContainerID containerId1 = *iterator;
  const ContainerID containerId2 = *(++iterator);

  slave.get()->terminate();

  // Wipe the slave meta directory so that the slave will treat the
  // above running tasks as orphans.
  ASSERT_SOME(os::rmdir(paths::getMetaRootDir(flags.work_dir)));

  // Remove the network namespace symlink for one container so that it
  // becomes an unknown orphan.
  const string symlink = path::join(
      slave::PORT_MAPPING_BIND_MOUNT_SYMLINK_ROOT(),
      stringify(containerId1));

  ASSERT_TRUE(os::exists(symlink));
  ASSERT_TRUE(os::stat::islink(symlink));
  ASSERT_SOME(os::rm(symlink));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Destroy the old containerizer first so that it won't remove new
  // containerizer's metrics.
  containerizer.reset();

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);

  containerizer.reset(_containerizer.get());

  // Restart the slave.
  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Wait until slave recovery is complete.
  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);
  AWAIT_READY_FOR(_recover, Seconds(60));

  // Wait until the orphan containers are cleaned up.
  AWAIT_READY_FOR(containerizer.get()->wait(containerId2), Seconds(60));
  AWAIT_READY(slaveRegisteredMessage);

  // We settle the clock here to ensure that the processing of
  // 'MesosContainerizerProcess::___destroy()' is complete and the
  // metric is updated.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      0u,
      metrics.values["containerizer/mesos/container_destroy_errors"]);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
