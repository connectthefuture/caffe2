#include "caffe2/core/workspace.h"

#include <algorithm>
#include <ctime>
#include <mutex>

#include "caffe2/core/logging.h"
#include "caffe2/core/net.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/plan_executor.h"
#include "caffe2/core/tensor.h"
#include "caffe2/proto/caffe2.pb.h"

CAFFE2_DEFINE_bool(
    caffe2_print_blob_sizes_at_exit,
    false,
    "If true, workspace destructor will print all blob shapes");

#if CAFFE2_MOBILE
// Threadpool restrictions

// Whether or not threadpool caps apply to Android
CAFFE2_DEFINE_int(caffe2_threadpool_android_cap, true, "");

// Whether or not threadpool caps apply to iOS
CAFFE2_DEFINE_int(caffe2_threadpool_ios_cap, false, "");

#endif // CAFFE2_MOBILE

namespace caffe2 {

void Workspace::PrintBlobSizes() {
  vector<string> blobs = LocalBlobs();
  size_t cumtotal = 0;

  // First get total sizes and sort
  vector<std::pair<size_t, std::string>> blob_sizes;
  for (const auto& s : blobs) {
    Blob* b = this->GetBlob(s);
    ShapeCall shape_fun = GetShapeCallFunction(b->meta().id());
    if (shape_fun) {
      bool shares_data = false;
      size_t capacity;
      auto shape = shape_fun(b->GetRaw(), shares_data, capacity);
      if (shares_data) {
        // Blobs sharing data do not actually take any memory
        capacity = 0;
      }
      cumtotal += capacity;
      blob_sizes.push_back(make_pair(capacity, s));
    }
  }
  std::sort(
      blob_sizes.begin(),
      blob_sizes.end(),
      [](const std::pair<size_t, std::string>& a,
         const std::pair<size_t, std::string>& b) {
        return b.first < a.first;
      });

  // Then print in descending order
  LOG(INFO) << "---- Workspace blobs: ---- ";
  LOG(INFO) << "name;current shape;capacity bytes;percentage";
  for (const auto& sb : blob_sizes) {
    Blob* b = this->GetBlob(sb.second);
    ShapeCall shape_fun = GetShapeCallFunction(b->meta().id());
    CHECK(shape_fun != nullptr);
    bool _shares_data = false;
    size_t capacity;
    auto shape = shape_fun(b->GetRaw(), _shares_data, capacity);
    std::stringstream ss;
    ss << sb.second << ";";
    for (const auto d : shape) {
      ss << d << ",";
    }
    LOG(INFO) << ss.str() << ";" << sb.first << ";" << std::setprecision(3)
              << (cumtotal > 0 ? 100.0 * double(sb.first) / cumtotal : 0.0)
              << "%";
  }
  LOG(INFO) << "Total;;" << cumtotal << ";100%";
}

vector<string> Workspace::LocalBlobs() const {
  vector<string> names;
  for (auto& entry : blob_map_) {
    names.push_back(entry.first);
  }
  return names;
}

vector<string> Workspace::Blobs() const {
  vector<string> names;
  for (auto& entry : blob_map_) {
    names.push_back(entry.first);
  }
  if (shared_) {
    vector<string> shared_blobs = shared_->Blobs();
    names.insert(names.end(), shared_blobs.begin(), shared_blobs.end());
  }
  return names;
}

Blob* Workspace::CreateBlob(const string& name) {
  if (HasBlob(name)) {
    VLOG(1) << "Blob " << name << " already exists. Skipping.";
  } else {
    VLOG(1) << "Creating blob " << name;
    blob_map_[name] = unique_ptr<Blob>(new Blob());
  }
  return GetBlob(name);
}

bool Workspace::RemoveBlob(const string& name) {
  auto it = blob_map_.find(name);
  if (it != blob_map_.end()) {
    VLOG(1) << "Removing blob " << name << " from this workspace.";
    blob_map_.erase(it);
    return true;
  }

  // won't go into share_ here
  VLOG(1) << "Blob " << name << " not exists. Skipping.";
  return false;
}

const Blob* Workspace::GetBlob(const string& name) const {
  if (blob_map_.count(name)) {
    return blob_map_.at(name).get();
  } else if (shared_ && shared_->HasBlob(name)) {
    return shared_->GetBlob(name);
  } else {
    LOG(WARNING) << "Blob " << name << " not in the workspace.";
    // TODO(Yangqing): do we want to always print out the list of blobs here?
    // LOG(WARNING) << "Current blobs:";
    // for (const auto& entry : blob_map_) {
    //   LOG(WARNING) << entry.first;
    // }
    return nullptr;
  }
}

Blob* Workspace::GetBlob(const string& name) {
  return const_cast<Blob*>(
      static_cast<const Workspace*>(this)->GetBlob(name));
}

NetBase* Workspace::CreateNet(const NetDef& net_def, bool overwrite) {
  CAFFE_ENFORCE(net_def.has_name(), "Net definition should have a name.");
  if (net_map_.count(net_def.name()) > 0) {
    if (!overwrite) {
      CAFFE_THROW(
          "I respectfully refuse to overwrite an existing net of the same "
          "name \"",
          net_def.name(),
          "\", unless you explicitly specify overwrite=true.");
    }
    VLOG(1) << "Deleting existing network of the same name.";
    // Note(Yangqing): Why do we explicitly erase it here? Some components of
    // the old network, such as an opened LevelDB, may prevent us from creating
    // a new network before the old one is deleted. Thus we will need to first
    // erase the old one before the new one can be constructed.
    net_map_.erase(net_def.name());
  }
  // Create a new net with its name.
  VLOG(1) << "Initializing network " << net_def.name();
  net_map_[net_def.name()] =
      unique_ptr<NetBase>(caffe2::CreateNet(net_def, this));
  if (net_map_[net_def.name()].get() == nullptr) {
    LOG(ERROR) << "Error when creating the network."
               << "Maybe net type: [" << net_def.type() << "] does not exist";
    net_map_.erase(net_def.name());
    return nullptr;
  }
  return net_map_[net_def.name()].get();
}

NetBase* Workspace::GetNet(const string& name) {
  if (!net_map_.count(name)) {
    return nullptr;
  } else {
    return net_map_[name].get();
  }
}

void Workspace::DeleteNet(const string& name) {
  if (net_map_.count(name)) {
    net_map_.erase(name);
  }
}

bool Workspace::RunNet(const string& name) {
  if (!net_map_.count(name)) {
    LOG(ERROR) << "Network " << name << " does not exist yet.";
    return false;
  }
  return net_map_[name]->Run();
}

bool Workspace::RunOperatorOnce(const OperatorDef& op_def) {
  std::unique_ptr<OperatorBase> op(CreateOperator(op_def, this));
  if (op.get() == nullptr) {
    LOG(ERROR) << "Cannot create operator of type " << op_def.type();
    return false;
  }
  if (!op->Run()) {
    LOG(ERROR) << "Error when running operator " << op_def.type();
    return false;
  }
  return true;
}
bool Workspace::RunNetOnce(const NetDef& net_def) {
  std::unique_ptr<NetBase> net(caffe2::CreateNet(net_def, this));
  if (net == nullptr) {
    CAFFE_THROW(
        "Could not create net: " + net_def.name() + " of type " +
        net_def.type());
  }
  if (!net->Run()) {
    LOG(ERROR) << "Error when running network " << net_def.name();
    return false;
  }
  return true;
}

bool Workspace::RunPlan(const PlanDef& plan,
                        ShouldContinue shouldContinue) {
  return RunPlanOnWorkspace(this, plan, shouldContinue);
}

#if CAFFE2_MOBILE
ThreadPool* Workspace::GetThreadPool() {
  std::lock_guard<std::mutex> guard(thread_pool_creation_mutex_);

  if (!thread_pool_) {
    int numThreads = std::thread::hardware_concurrency();

    bool applyCap = false;
#if CAFFE2_ANDROID
    applyCap = caffe2::FLAGS_caffe2_threadpool_android_cap;
#elif CAFFE2_IOS
    applyCap = caffe2::FLAGS_caffe2_threadpool_ios_cap;
#else
#error Undefined architecture
#endif

    if (applyCap) {
      // 1 core  -> 1 thread
      // 2 cores -> 2 threads
      // 4 cores -> 3 threads
      // 8 cores -> 4 threads
      // more, continue limiting to half of available cores

      if (numThreads <= 3) {
        // no change
      } else if (numThreads <= 5) {
        // limit to 3
        numThreads = 3;
      } else {
        // Use half the cores
        numThreads = numThreads / 2;
      }
    }

    LOG(INFO) << "Constructing thread pool with " << numThreads << " threads";
    thread_pool_.reset(new ThreadPool(numThreads));
  }

  return thread_pool_.get();
}
#endif // CAFFE2_MOBILE

}  // namespace caffe2
