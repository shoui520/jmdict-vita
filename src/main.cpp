#include "dictionary.h"

#include <limits>
#include <paf.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>

char sceUserMainThreadName[] = "jmdict_vita";
int sceUserMainThreadPriority = 0x10000100;
// Process-image system applications may only run on the system-reserved CPU3.
int sceUserMainThreadCpuAffinityMask = SCE_KERNEL_CPU_MASK_SYSTEM;
SceSize sceUserMainThreadStackSize = 0x4000;

// ScePaf owns allocation in the component's no-CRT process-image layout.
void operator delete(void *pointer, unsigned int) {
	sce_paf_free(pointer);
}

namespace {

static const char kDictionaryPath[] = "app0:/dictionary/jmdict_vita.bin";

paf::Framework *g_framework = NULL;
paf::ui::Scene *g_scene = NULL;
paf::ui::TextBox *g_search_box = NULL;
paf::ui::Text *g_status = NULL;
paf::ui::RichText *g_results = NULL;
bool g_showing_sources = false;

class LookupWorker : public paf::thread::Thread {
public:
	LookupWorker()
		: paf::thread::Thread(SCE_KERNEL_PROCESS_PRIORITY_USER_DEFAULT, 0x20000, "JmdictLookup"),
		  mutex_("JmdictLookupMutex"), wake_("JmdictLookupWake", 0, 1),
		  work_markup_(NULL), published_markup_(NULL), submitted_serial_(0),
		  published_length_(0), pending_(false), ready_(false), stopping_(false),
		  started_(false), open_result_(-1) {
		jmdict::Reset(&engine_);
		sce_paf_memset(pending_query_, 0, sizeof(pending_query_));
		work_markup_ = static_cast<char *>(sce_paf_malloc(jmdict::RESULT_MARKUP_MAX));
		published_markup_ = static_cast<char *>(sce_paf_malloc(jmdict::RESULT_MARKUP_MAX));
		if (work_markup_ && published_markup_) {
			work_markup_[0] = 0;
			published_markup_[0] = 0;
			open_result_ = jmdict::Open(&engine_, kDictionaryPath);
			if (open_result_ == 0) {
				int start_result = Start();
				if (start_result == 0) {
					started_ = true;
				} else {
					open_result_ = start_result;
					jmdict::Close(&engine_);
				}
			}
		} else {
			open_result_ = -4;
		}
	}

	~LookupWorker() {
		jmdict::Close(&engine_);
		if (work_markup_) sce_paf_free(work_markup_);
		if (published_markup_) sce_paf_free(published_markup_);
	}

	bool IsReady() const { return open_result_ == 0; }
	int OpenResult() const { return open_result_; }

	void Submit(const char *query) {
		bool signal = false;
		mutex_.Lock();
		sce_paf_strlcpy(pending_query_, query, sizeof(pending_query_));
		++submitted_serial_;
		if (!pending_) {
			pending_ = true;
			signal = true;
		}
		mutex_.Unlock();
		if (signal) wake_.Release();
	}

	bool ApplyPublished(paf::ui::RichText *target) {
		bool applied = false;
		mutex_.Lock();
		if (ready_ && target) {
			target->Clear();
			target->SetCText(published_markup_, published_length_);
			target->SetVisibleTop(0.0f);
			ready_ = false;
			applied = true;
		}
		mutex_.Unlock();
		return applied;
	}

	void DiscardPublished() {
		mutex_.Lock();
		ready_ = false;
		mutex_.Unlock();
	}

	void StopAndJoin() {
		if (!started_) return;
		bool signal = false;
		mutex_.Lock();
		stopping_ = true;
		if (!pending_) {
			pending_ = true;
			signal = true;
		}
		mutex_.Unlock();
		if (signal) wake_.Release();
		Join();
	}

	void EntryFunction() {
		for (;;) {
			wake_.Acquire();
			char query[jmdict::QUERY_UTF8_MAX];
			uint32_t serial;
			mutex_.Lock();
			if (stopping_) {
				mutex_.Unlock();
				break;
			}
			sce_paf_strlcpy(query, pending_query_, sizeof(query));
			serial = submitted_serial_;
			pending_ = false;
			mutex_.Unlock();

			jmdict::SearchStats stats;
			int result = jmdict::Search(&engine_, query, work_markup_, jmdict::RESULT_MARKUP_MAX, &stats);
			if (result < 0) {
				sce_paf_strlcpy(work_markup_,
					"<font color=\"#ff8080\">Dictionary read error.</font>",
					jmdict::RESULT_MARKUP_MAX);
			}

			mutex_.Lock();
			if (!stopping_ && serial == submitted_serial_) {
				published_length_ = sce_paf_strlcpy(published_markup_, work_markup_,
					jmdict::RESULT_MARKUP_MAX);
				if (published_length_ >= jmdict::RESULT_MARKUP_MAX)
					published_length_ = jmdict::RESULT_MARKUP_MAX - 1;
				ready_ = true;
			}
			mutex_.Unlock();
		}
		Cancel();
	}

private:
	paf::thread::Mutex mutex_;
	paf::thread::Semaphore wake_;
	jmdict::Engine engine_;
	char *work_markup_;
	char *published_markup_;
	char pending_query_[jmdict::QUERY_UTF8_MAX];
	uint32_t submitted_serial_;
	size_t published_length_;
	bool pending_;
	bool ready_;
	bool stopping_;
	bool started_;
	int open_result_;
};

LookupWorker *g_worker = NULL;

void PumpPublishedResult(void *) {
	if (!g_showing_sources && g_worker && g_worker->ApplyPublished(g_results) && g_status)
		g_status->SetString(L"Search complete");
}

void OnSearchSubmitted(int, paf::ui::Handler *caller, paf::ui::Event *, void *) {
	paf::ui::TextBox *text_box = static_cast<paf::ui::TextBox *>(caller);
	paf::wstring query16;
	char query8[jmdict::QUERY_UTF8_MAX];
	text_box->GetString(query16);
	size_t query_length = jmdict::NormalizeQuery(query16.c_str(), query16.size(), query8, sizeof(query8));
	text_box->EndEdit();

	if (!g_worker || !g_worker->IsReady()) return;
	g_showing_sources = false;
	g_worker->DiscardPublished();
	if (!query_length) {
		static const char help[] =
			"<font color=\"#c8d4e8\">Type a Japanese word, reading, name, or English gloss, then press Enter.</font>";
		if (g_results) {
			g_results->Clear();
			g_results->SetCText(help, sizeof(help) - 1);
		}
		if (g_status) g_status->SetString(L"Ready");
		return;
	}

	if (g_status) g_status->SetString(L"Searching…");
	g_worker->Submit(query8);
}

void OnSourcesPressed(int, paf::ui::Handler *, paf::ui::Event *, void *) {
	static const char sources[] =
		"<h1><font color=\"#ffffff\">Sources &amp; licence</font></h1>"
		"<font color=\"#c8d4e8\">Dictionary data</font><br/>"
		"JMdict and JMnedict files are the property of the Electronic Dictionary "
		"Research and Development Group and are used in conformance with the Group's licence.<br/><br/>"
		"The dictionary data and the derived on-device index are licensed under the "
		"Creative Commons Attribution-ShareAlike 4.0 International licence.<br/>"
		"<font color=\"#8fd5c4\">https://www.edrdg.org/edrdg/licence.html</font><br/>"
		"<font color=\"#8fd5c4\">https://creativecommons.org/licenses/by-sa/4.0/</font><br/><br/>"
		"<font color=\"#c8d4e8\">Application</font><br/>"
		"JMdict Vita performs all lookups locally and does not modify the source data.";
	g_showing_sources = true;
	if (g_worker) g_worker->DiscardPublished();
	if (g_results) {
		g_results->Clear();
		g_results->SetCText(sources, sizeof(sources) - 1);
		g_results->SetVisibleTop(0.0f);
	}
	if (g_status) g_status->SetString(L"Sources & licence");
}

void OnPluginLoaded(paf::Plugin *plugin) {
	paf::Plugin::PageOpenParam page_param;
	page_param.option = paf::Plugin::PageOption_None;
	g_scene = plugin->PageOpen("page_main", page_param);
	if (!g_scene) return;

	g_search_box = static_cast<paf::ui::TextBox *>(g_scene->FindChild("search_box"));
	g_status = static_cast<paf::ui::Text *>(g_scene->FindChild("status_text"));
	g_results = static_cast<paf::ui::RichText *>(g_scene->FindChild("result_text"));
	paf::ui::Text *title = static_cast<paf::ui::Text *>(g_scene->FindChild("title_text"));
	paf::ui::Text *hint = static_cast<paf::ui::Text *>(g_scene->FindChild("hint_text"));
	paf::ui::Widget *sources_button = g_scene->FindChild("sources_button");

	if (title) title->SetString(L"JMdict");
	if (hint) hint->SetString(L"Tap the search box · Enter to search");
	if (sources_button) {
		sources_button->SetString(L"Sources");
		sources_button->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE,
			static_cast<paf::ui::HandlerCB>(OnSourcesPressed), NULL);
	}
	if (g_status) g_status->SetString(L"Opening dictionary…");

	g_worker = new LookupWorker();
	if (g_search_box) {
		g_search_box->SetMaxLength(96);
		g_search_box->AddEventCallback(paf::ui::TextBox::CB_TEXT_BOX_ENTER_PRESSED,
			static_cast<paf::ui::HandlerCB>(OnSearchSubmitted), NULL);
	}

	if (g_worker && g_worker->IsReady()) {
		static const char help[] =
			"<font color=\"#c8d4e8\">Search JMdict and JMnedict without loading either dictionary into memory.</font>";
		if (g_results) g_results->SetCText(help, sizeof(help) - 1);
		if (g_status) g_status->SetString(L"Ready");
	} else {
		static const char error[] =
			"<font color=\"#ff8080\">Dictionary unavailable.</font><br/><br/>"
			"Build and package <font color=\"#ffffff\">build/jmdict_vita.bin</font>.";
		if (g_results) g_results->SetCText(error, sizeof(error) - 1);
		if (g_status) g_status->SetString(L"Dictionary error");
	}

	paf::common::MainThreadCallList::Register(PumpPublishedResult, NULL);
}

int RunPafApplication() {
	paf::Framework::InitParam framework_param;
	// Retail PAF system applications (Browser and Settings) use application
	// mode and the VSH graphics path.  Mode_Normal requests the non-sysapp GXM
	// flags and leaves GXM uninitialized under a system-application FSELF.
	framework_param.screen_width = 960;
	framework_param.screen_height = 544;
	framework_param.surface_pool_size = 0x002C0000;
	framework_param.text_surface_pool_size = 0x00080000;
	framework_param.mode = paf::Framework::Mode_Application;
	framework_param.allow_button_control = true;
	framework_param.graphics_option = 7;

	g_framework = new paf::Framework(framework_param);
	if (!g_framework) return -1;
	g_framework->LoadCommonResourceSync();

	paf::Plugin::InitParam plugin_param;
	plugin_param.name = "jmdict_vita";
	plugin_param.caller_name = "__main__";
	plugin_param.resource_file = "app0:/jmdict_vita.rco";
	plugin_param.init_func = NULL;
	plugin_param.start_func = OnPluginLoaded;
	plugin_param.stop_func = NULL;
	plugin_param.exit_func = NULL;

	paf::Plugin::LoadSync(plugin_param);
	g_framework->Run();

	paf::common::MainThreadCallList::Unregister(PumpPublishedResult, NULL);
	if (g_worker) {
		g_worker->StopAndJoin();
		delete g_worker;
		g_worker = NULL;
	}
	return 0;
}

} // namespace

extern "C" {

struct ScePafInit {
	SceSize global_heap_size;
	int a2;
	int a3;
	int cdlg_mode;
	int heap_opt_param1;
	int heap_opt_param2;
};

int module_start(SceSize, void *) {
	ScePafInit init_param;
	// Match the component's Sony-SDK launcher. Dictionary lookup adds less than
	// 0.5 MiB of bounded allocations inside this heap.
	init_param.global_heap_size = 0x00800000;
	init_param.a2 = 0xEA60;
	init_param.a3 = 0x40000;
	init_param.cdlg_mode = 0;
	init_param.heap_opt_param1 = 0;
	init_param.heap_opt_param2 = 0;

	int load_result = 0xDEADBEEF;
	SceSysmoduleOpt sysmodule_opt;
	sceClibMemset(&sysmodule_opt, 0, sizeof(sysmodule_opt));
	sysmodule_opt.result = &load_result;
	int result = sceSysmoduleLoadModuleInternalWithArg(
		SCE_SYSMODULE_INTERNAL_PAF, sizeof(init_param), &init_param, &sysmodule_opt);
	if ((result | load_result) != 0) return SCE_KERNEL_START_FAILED;

	RunPafApplication();
	return SCE_KERNEL_START_SUCCESS;
}

} // extern "C"
