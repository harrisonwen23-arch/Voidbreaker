#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <algorithm>
#include <ctime>
#include <functional>
#include <sstream>

using namespace geode::prelude;

// ---------------------------------------------------------------- state ----
namespace vb {
	struct Input { int frame; bool down; int button; bool p1; };

	struct WindowEntry {
		int startFrame = 0;
		int endFrame = 0;
		int frames = 0;
		int gapFrames = 0;
		int button = 0;
		bool p1 = true;
	};

	struct WindowStats {
		int count = 0;
		int minFrames = 0;
		int maxFrames = 0;
		float avgFrames = 0.f;
		int lastFrames = 0;
		int lastGapFrames = 0;
		std::vector<WindowEntry> entries;
	};

	struct State {
		bool disabled = false;
		bool hitboxes = false;
		bool trajectory = false;
		bool hudFps = false;
		bool hudMacro = false;
		bool quick = false;
		float speed = 1.f;
		float tps = 240.f;
		float trajSteps = 100.f;
		float btnScale = 1.f;
		float hudScale = 0.6f;

		int frame = 0;
		bool recording = false;
		bool playing = false;
		bool injecting = false;
		bool simulating = false;
		size_t idx = 0;
		std::vector<Input> macro;
	};

	struct SimGuard {
		bool& flag;
		explicit SimGuard(bool& f) : flag(f) { flag = true; }
		~SimGuard() { flag = false; }
	};

	inline State& S() { static State s; return s; }
	inline WindowStats& W() { static WindowStats w; return w; }

	inline void measureMacro() {
		auto const& s = S();
		auto& w = W();
		w.entries.clear();
		w.count = 0;
		w.minFrames = 0;
		w.maxFrames = 0;
		w.avgFrames = 0.f;
		w.lastFrames = 0;
		w.lastGapFrames = 0;

		if (s.macro.empty()) return;
		w.entries.reserve(s.macro.size() / 2 + 1);

		struct ActivePress {
			bool active = false;
			int startFrame = 0;
			int lastReleaseFrame = -1;
			bool hasReleased = false;
		};
		ActivePress press[2][8];

		int totalFrames = 0;
		for (auto const& in : s.macro) {
			int pIdx = in.p1 ? 0 : 1;
			int bIdx = std::clamp(in.button, 0, 7);
			auto& slot = press[pIdx][bIdx];

			if (in.down) {
				if (!slot.active) {
					slot.active = true;
					slot.startFrame = in.frame;
				}
			} else if (slot.active) {
				int win = std::max(1, in.frame - slot.startFrame);
				int gap = slot.hasReleased ? std::max(0, slot.startFrame - slot.lastReleaseFrame) : 0;
				w.entries.push_back({slot.startFrame, in.frame, win, gap, in.button, in.p1});

				if (w.count == 0) {
					w.minFrames = win;
					w.maxFrames = win;
				} else {
					w.minFrames = std::min(w.minFrames, win);
					w.maxFrames = std::max(w.maxFrames, win);
				}
				totalFrames += win;
				w.count++;
				w.lastFrames = win;
				w.lastGapFrames = gap;
				slot.lastReleaseFrame = in.frame;
				slot.hasReleased = true;
				slot.active = false;
			}
		}

		if (w.count > 0) {
			w.avgFrames = static_cast<float>(totalFrames) / static_cast<float>(w.count);
		}
	}

	inline void load() {
		auto m = Mod::get();
		auto& s = S();
		s.hitboxes = m->getSavedValue<bool>("hitboxes", false);
		s.trajectory = m->getSavedValue<bool>("trajectory", false);
		s.hudFps = m->getSavedValue<bool>("hudFps", false);
		s.hudMacro = m->getSavedValue<bool>("hudMacro", false);
		s.quick = m->getSavedValue<bool>("quick", false);
		s.speed = m->getSavedValue<float>("speed", 1.f);
		s.tps = m->getSavedValue<float>("tps", 240.f);
		s.trajSteps = m->getSavedValue<float>("trajSteps", 100.f);
		s.btnScale = m->getSavedValue<float>("btnScale", 1.f);
		s.hudScale = m->getSavedValue<float>("hudScale", 0.6f);
	}

	inline void save() {
		auto m = Mod::get();
		auto& s = S();
		m->setSavedValue("hitboxes", s.hitboxes);
		m->setSavedValue("trajectory", s.trajectory);
		m->setSavedValue("hudFps", s.hudFps);
		m->setSavedValue("hudMacro", s.hudMacro);
		m->setSavedValue("quick", s.quick);
		m->setSavedValue("speed", s.speed);
		m->setSavedValue("tps", s.tps);
		m->setSavedValue("trajSteps", s.trajSteps);
		m->setSavedValue("btnScale", s.btnScale);
		m->setSavedValue("hudScale", s.hudScale);
	}

	inline std::filesystem::path macroDir() { return Mod::get()->getSaveDir() / "macros"; }

	inline std::string serialize() {
		std::ostringstream o;
		for (auto& i : S().macro) o << i.frame << ' ' << (int)i.down << ' ' << i.button << ' ' << (int)i.p1 << '\n';
		return o.str();
	}

	inline void deserialize(std::string const& text) {
		auto& m = S().macro;
		m.clear();
		std::istringstream in(text);
		int f, d, b, p;
		while (in >> f >> d >> b >> p) m.push_back({f, d != 0, b, p != 0});
	}

	inline void rewindMacro(int frame) {
		auto& s = S();
		s.idx = std::lower_bound(s.macro.begin(), s.macro.end(), frame,
			[](Input const& i, int f) { return i.frame < f; }) - s.macro.begin();
	}
}

$on_mod(Loaded) { vb::load(); }

static CCSprite* loadLogo() {
	if (auto s = CCSprite::create("logo-voidbreaker.png"_spr)) {
		if (s->getContentSize().width > 0.f) return s;
	}
	return nullptr;
}

// ----------------------------------------------------------------- menu ----
class VBMenu : public geode::Popup {
protected:
	struct Stepper {
		std::function<float()> get;
		std::function<void(float)> set;
		float mn, mx, st;
		int dec;
		CCLabelBMFont* lbl;
	};

	CCNode* m_pages[6] = {};
	CCNode* m_sub[2] = {};
	CCNode* m_macroList = nullptr;
	CCMenu* m_macroMenu = nullptr;
	ButtonSprite* m_recBtn = nullptr;
	ButtonSprite* m_playBtn = nullptr;
	ButtonSprite* m_disBtn = nullptr;
	CCLabelBMFont* m_status = nullptr;
	std::vector<bool*> m_targets;
	std::vector<Stepper> m_steppers;
	std::vector<std::filesystem::path> m_files;
	int m_current = 0;

	CCNode* newPage(CCMenu*& menu) {
		auto page = CCNode::create();
		menu = CCMenu::create();
		menu->setPosition({0.f, 0.f});
		page->addChild(menu, 1);
		page->setVisible(false);
		m_mainLayer->addChild(page, 2);
		return page;
	}

	CCLabelBMFont* addText(CCNode* parent, char const* text, CCPoint pos, float scale, bool left = false) {
		auto l = CCLabelBMFont::create(text, "bigFont.fnt");
		l->setScale(scale);
		if (left) l->setAnchorPoint({0.f, 0.5f});
		l->setPosition(pos);
		parent->addChild(l);
		return l;
	}

	void addToggle(CCNode* page, CCMenu* menu, float y, char const* text, bool* target) {
		auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(VBMenu::onToggle), .6f);
		t->setPosition({128.f, y});
		t->toggle(*target);
		t->setTag((int)m_targets.size());
		m_targets.push_back(target);
		menu->addChild(t);
		addText(page, text, {146.f, y}, .4f, true);
	}

	void addStepper(CCNode* page, CCMenu* menu, float y, char const* text,
		std::function<float()> get, std::function<void(float)> set, float mn, float mx, float st, int dec) {
		int idx = (int)m_steppers.size();
		addText(page, text, {118.f, y}, .35f, true);

		auto l = CCSprite::createWithSpriteFrameName("edit_leftBtn_001.png");
		auto r = CCSprite::createWithSpriteFrameName("edit_rightBtn_001.png");
		l->setScale(.7f);
		r->setScale(.7f);
		auto lb = CCMenuItemSpriteExtra::create(l, this, menu_selector(VBMenu::onStep));
		auto rb = CCMenuItemSpriteExtra::create(r, this, menu_selector(VBMenu::onStep));
		lb->setTag(idx * 2);
		rb->setTag(idx * 2 + 1);
		lb->setPosition({262.f, y});
		rb->setPosition({322.f, y});
		menu->addChild(lb);
		menu->addChild(rb);

		auto lbl = addText(page, fmt::format("{:.{}f}", get(), dec).c_str(), {292.f, y}, .4f);
		m_steppers.push_back({get, set, mn, mx, st, dec, lbl});
	}

	ButtonSprite* addButton(CCMenu* menu, char const* text, CCPoint pos, SEL_MenuHandler cb, int w = 90, int tag = 0) {
		auto spr = ButtonSprite::create(text, w, true, "bigFont.fnt", "GJ_button_04.png", 28.f, .55f);
		auto b = CCMenuItemSpriteExtra::create(spr, this, cb);
		b->setPosition(pos);
		b->setTag(tag);
		menu->addChild(b);
		return spr;
	}

	bool init() {
		if (!Popup::init(440.f, 280.f)) return false;
		this->setTitle("Voidbreaker");
		auto W = m_size.width;
		auto H = m_size.height;
		auto& s = vb::S();

		addText(m_mainLayer, "Created by harrison", {W / 2.f, H - 38.f}, .38f);

		auto tabs = CCMenu::create();
		tabs->setPosition({0.f, 0.f});
		m_mainLayer->addChild(tabs, 3);
		char const* names[5] = {"Replay", "Hacks", "Macros", "Windows", "Settings"};
		for (int i = 0; i < 5; i++) {
			addButton(tabs, names[i], {58.f, H - 70.f - i * 42.f}, menu_selector(VBMenu::onTab), 80, i + 1);
		}

		auto right = CCMenu::create();
		right->setPosition({0.f, 0.f});
		m_mainLayer->addChild(right, 3);
		m_disBtn = addButton(right, "Disable", {W - 58.f, H - 70.f}, menu_selector(VBMenu::onDisable), 80);
		m_recBtn = addButton(right, "Record", {W - 58.f, H - 112.f}, menu_selector(VBMenu::onRecord), 80);
		m_playBtn = addButton(right, "Playback", {W - 58.f, H - 154.f}, menu_selector(VBMenu::onPlayback), 80);

		CCMenu* menu = nullptr;

		// page 0: home / logo
		m_pages[0] = newPage(menu);
		if (auto logo = loadLogo()) {
			logo->setScale(140.f / logo->getContentSize().width);
			logo->setPosition({W / 2.f, 120.f});
			m_pages[0]->addChild(logo);
		} else {
			addText(m_pages[0], "Voidbreaker", {W / 2.f, 120.f}, .9f);
		}

		// page 1: replay
		m_pages[1] = newPage(menu);
		m_status = addText(m_pages[1], "", {W / 2.f, 175.f}, .34f);
		addButton(menu, "Clear macro", {W / 2.f, 120.f}, menu_selector(VBMenu::onClear), 110);
		addText(m_pages[1], "Measures click windows per frame.", {W / 2.f, 75.f}, .3f);

		// page 2: hacks
		m_pages[2] = newPage(menu);
		addToggle(m_pages[2], menu, 195.f, "Show hitboxes", &s.hitboxes);
		addToggle(m_pages[2], menu, 165.f, "Show trajectory", &s.trajectory);
		addStepper(m_pages[2], menu, 130.f, "Speedhack", [] { return vb::S().speed; },
			[](float v) { vb::S().speed = v; }, .1f, 5.f, .1f, 2);
		addStepper(m_pages[2], menu, 100.f, "TPS", [] { return vb::S().tps; },
			[](float v) { vb::S().tps = v; }, 30.f, 960.f, 30.f, 0);
		addStepper(m_pages[2], menu, 70.f, "Trajectory steps", [] { return vb::S().trajSteps; },
			[](float v) { vb::S().trajSteps = v; }, 20.f, 300.f, 20.f, 0);

		// page 3: macros
		m_pages[3] = newPage(menu);
		addButton(menu, "Save current", {W / 2.f, 195.f}, menu_selector(VBMenu::onSave), 120);
		m_macroList = CCNode::create();
		m_macroMenu = CCMenu::create();
		m_macroMenu->setPosition({0.f, 0.f});
		m_macroList->addChild(m_macroMenu, 1);
		m_pages[3]->addChild(m_macroList);
		refreshMacros();

		// page 4: windows (overlays)
		m_pages[4] = newPage(menu);
		addToggle(m_pages[4], menu, 195.f, "FPS overlay", &s.hudFps);
		addToggle(m_pages[4], menu, 165.f, "Macro overlay", &s.hudMacro);
		addStepper(m_pages[4], menu, 130.f, "Overlay size", [] { return vb::S().hudScale; },
			[](float v) { vb::S().hudScale = v; }, .3f, 1.2f, .1f, 1);

		// page 5: settings with sub tabs
		m_pages[5] = newPage(menu);
		addButton(menu, "Interface", {170.f, 205.f}, menu_selector(VBMenu::onSub), 80, 0);
		addButton(menu, "Keybinds", {270.f, 205.f}, menu_selector(VBMenu::onSub), 80, 1);

		CCMenu* m1 = nullptr;
		m_sub[0] = newPage(m1);
		addStepper(m_sub[0], m1, 160.f, "Button size", [] { return vb::S().btnScale; },
			[](float v) { vb::S().btnScale = v; }, .5f, 1.5f, .1f, 1);
		addText(m_sub[0], "Applies next time a menu opens.", {W / 2.f, 120.f}, .3f);

		CCMenu* m2 = nullptr;
		m_sub[1] = newPage(m2);
		addToggle(m_sub[1], m2, 160.f, "Quick buttons", &s.quick);
		addText(m_sub[1], "On-screen REC / PLAY / HB / TRJ", {W / 2.f, 125.f}, .3f);
		addText(m_sub[1], "buttons in level (touch keybinds).", {W / 2.f, 110.f}, .3f);

		showPage(0);
		showSub(0);
		this->schedule(schedule_selector(VBMenu::tick), .2f);
		tick(0.f);
		return true;
	}

	void showPage(int p) {
		m_current = p;
		for (int i = 0; i < 6; i++) m_pages[i]->setVisible(i == p);
		if (p == 5) showSub(0);
		else for (auto n : m_sub) n->setVisible(false);
	}

	void showSub(int i) {
		for (int k = 0; k < 2; k++) m_sub[k]->setVisible(m_current == 5 && k == i);
	}

	void refreshMacros() {
		m_macroMenu->removeAllChildren();
		while (m_macroList->getChildrenCount() > 1) {
			for (auto c : CCArrayExt<CCNode*>(m_macroList->getChildren())) {
				if (c != m_macroMenu) { c->removeFromParent(); break; }
			}
		}
		m_files.clear();
		(void)geode::utils::file::createDirectoryAll(vb::macroDir());
		auto r = geode::utils::file::readDirectory(vb::macroDir());
		if (r.isOk()) {
			for (auto& p : r.unwrap()) if (p.extension() == ".vbm") m_files.push_back(p);
		}
		std::sort(m_files.begin(), m_files.end());
		float y = 160.f;
		for (size_t i = 0; i < m_files.size() && i < 5; i++, y -= 26.f) {
			auto name = m_files[i].stem().string();
			auto l = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
			l->setScale(.55f);
			l->setAnchorPoint({0.f, 0.5f});
			l->setPosition({118.f, y});
			m_macroList->addChild(l);
			addButton(m_macroMenu, "Load", {312.f, y}, menu_selector(VBMenu::onLoad), 44, (int)i);
		}
	}

	void tick(float) {
		auto& s = vb::S();
		auto const& w = vb::W();
		m_disBtn->setString(s.disabled ? "Enable" : "Disable");
		m_recBtn->setString(s.recording ? "Stop rec" : "Record");
		m_playBtn->setString(s.playing ? "Stop play" : "Playback");
		auto state = s.recording ? "RECORDING" : (s.playing ? "PLAYING" : "IDLE");
		if (w.count > 0) {
			float ms = (w.lastFrames * 1000.f) / std::max(1.f, s.tps);
			m_status->setString(fmt::format(
				"{} | {} win | last {}f ({:.1f}ms) | min {}f avg {:.1f}f",
				state, w.count, w.lastFrames, ms, w.minFrames, w.avgFrames
			).c_str());
		} else {
			m_status->setString(fmt::format("{} - {} inputs", state, s.macro.size()).c_str());
		}
	}

	void onTab(CCObject* o) {
		int p = static_cast<CCNode*>(o)->getTag();
		showPage(m_current == p ? 0 : p);
	}

	void onSub(CCObject* o) { showSub(static_cast<CCNode*>(o)->getTag()); }

	void onToggle(CCObject* o) {
		auto t = static_cast<CCMenuItemToggler*>(o);
		*m_targets[t->getTag()] = !t->isToggled();
		vb::save();
	}

	void onStep(CCObject* o) {
		int tag = static_cast<CCNode*>(o)->getTag();
		auto& st = m_steppers[tag / 2];
		float v = st.get() + ((tag % 2) ? st.st : -st.st);
		v = std::clamp(v, st.mn, st.mx);
		st.set(v);
		st.lbl->setString(fmt::format("{:.{}f}", v, st.dec).c_str());
		vb::save();
	}

	void onDisable(CCObject*) { vb::S().disabled = !vb::S().disabled; }

	void onRecord(CCObject*) {
		auto& s = vb::S();
		bool wasRecording = s.recording;
		s.recording = !s.recording;
		if (s.recording) {
			s.playing = false;
			s.macro.clear();
			s.idx = 0;
			vb::W() = {};
		} else if (wasRecording) {
			vb::measureMacro();
		}
	}

	void onPlayback(CCObject*) {
		auto& s = vb::S();
		s.playing = !s.playing;
		if (s.playing) { s.recording = false; vb::rewindMacro(s.frame); }
	}

	void onClear(CCObject*) {
		vb::S().macro.clear();
		vb::S().idx = 0;
		vb::W() = {};
	}

	void onSave(CCObject*) {
		if (vb::S().macro.empty()) {
			Notification::create("Nothing recorded yet", NotificationIcon::Warning)->show();
			return;
		}
		(void)geode::utils::file::createDirectoryAll(vb::macroDir());
		auto path = vb::macroDir() / fmt::format("macro-{}.vbm", (long long)std::time(nullptr));
		(void)geode::utils::file::writeString(path, vb::serialize());
		Notification::create("Macro saved", NotificationIcon::Success)->show();
		refreshMacros();
	}

	void onLoad(CCObject* o) {
		size_t i = (size_t)static_cast<CCNode*>(o)->getTag();
		if (i >= m_files.size()) return;
		auto r = geode::utils::file::readString(m_files[i]);
		if (r.isErr()) {
			Notification::create("Could not read macro", NotificationIcon::Error)->show();
			return;
		}
		vb::deserialize(r.unwrap());
		vb::rewindMacro(vb::S().frame);
		vb::measureMacro();
		Notification::create("Macro loaded", NotificationIcon::Success)->show();
	}

public:
	static VBMenu* create() {
		auto ret = new VBMenu();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

static CCMenuItemSpriteExtra* makeOpenButton(CCObject* target, SEL_MenuHandler cb) {
	CCNode* spr = loadLogo();
	if (spr) spr->setScale(34.f / spr->getContentSize().width * vb::S().btnScale);
	else spr = ButtonSprite::create("VB");
	return CCMenuItemSpriteExtra::create(spr, target, cb);
}

static void attachButton(CCNode* parent, char const* menuID, CCMenuItemSpriteExtra* btn) {
	if (auto m = parent->getChildByID(menuID)) {
		m->addChild(btn);
		m->updateLayout();
	} else {
		auto menu = CCMenu::create();
		menu->setPosition({28.f, 28.f});
		btn->setPosition({0.f, 0.f});
		menu->addChild(btn);
		parent->addChild(menu, 100);
	}
}

// ---------------------------------------------------- menu entry points ----
class $modify(VBMenuLayer, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;
		attachButton(this, "bottom-menu", makeOpenButton(this, menu_selector(VBMenuLayer::onVB)));
		return true;
	}
	void onVB(CCObject*) { VBMenu::create()->show(); }
};

class $modify(VBPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		attachButton(this, "right-button-menu", makeOpenButton(this, menu_selector(VBPause::onVB)));
	}
	void onVB(CCObject*) { VBMenu::create()->show(); }
};

// -------------------------------------------------------- quick buttons ----
class $modify(VBUILayer, UILayer) {
	bool init(GJBaseGameLayer* layer) {
		if (!UILayer::init(layer)) return false;
		if (!vb::S().quick) return true;
		auto menu = CCMenu::create();
		menu->setPosition({36.f, 105.f});
		char const* names[4] = {"REC", "PLAY", "HB", "TRJ"};
		for (int i = 0; i < 4; i++) {
			auto spr = ButtonSprite::create(names[i], 44, true, "bigFont.fnt", "GJ_button_04.png", 22.f, .45f);
			auto b = CCMenuItemSpriteExtra::create(spr, this, menu_selector(VBUILayer::onQuick));
			b->setTag(i + 1);
			b->setPosition({0.f, -i * 28.f});
			menu->addChild(b);
		}
		this->addChild(menu, 100);
		return true;
	}

	void onQuick(CCObject* o) {
		auto& s = vb::S();
		switch (static_cast<CCNode*>(o)->getTag()) {
			case 1: {
				bool wasRecording = s.recording;
				s.recording = !s.recording;
				if (s.recording) {
					s.playing = false;
					s.macro.clear();
					s.idx = 0;
					vb::W() = {};
				} else if (wasRecording) {
					vb::measureMacro();
				}
				break;
			}
			case 2: s.playing = !s.playing; if (s.playing) { s.recording = false; vb::rewindMacro(s.frame); } break;
			case 3: s.hitboxes = !s.hitboxes; break;
			case 4: s.trajectory = !s.trajectory; break;
		}
		vb::save();
	}
};

// ------------------------------------------------ speed / TPS + macros ----
class $modify(VBSched, CCScheduler) {
	void update(float dt) {
		auto& s = vb::S();
		if (!s.disabled && PlayLayer::get()) dt *= s.speed * (s.tps / 240.f);
		CCScheduler::update(dt);
	}
};

class $modify(VBBase, GJBaseGameLayer) {
	bool isPlay() {
		auto pl = PlayLayer::get();
		return pl && static_cast<GJBaseGameLayer*>(pl) == this;
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto& s = vb::S();
		if (isPlay()) {
			if (!s.disabled && s.playing) {
				while (s.idx < s.macro.size() && s.macro[s.idx].frame < s.frame) s.idx++;
				while (s.idx < s.macro.size() && s.macro[s.idx].frame == s.frame) {
					auto in = s.macro[s.idx++];
					s.injecting = true;
					this->handleButton(in.down, in.button, in.p1);
					s.injecting = false;
				}
			}
			s.frame++;
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	void handleButton(bool down, int button, bool isP1) {
		auto& s = vb::S();
		if (!s.disabled && s.recording && !s.injecting && isPlay()) {
			s.macro.push_back({s.frame, down, button, isP1});
		}
		GJBaseGameLayer::handleButton(down, button, isP1);
	}
};

// ------------------------------------ hitboxes / trajectory / overlays ----
class $modify(VBPlayLayer, PlayLayer) {
	struct Fields {
		CCDrawNode* draw = nullptr;
		PlayerObject* dummy = nullptr;
		CCLabelBMFont* hud = nullptr;
		float fps = 60.f;
		~Fields() { if (dummy) dummy->release(); }
	};

	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		vb::S().frame = 0;
		auto f = m_fields.self();
		f->draw = CCDrawNode::create();
		m_objectLayer->addChild(f->draw, 1000);

		f->dummy = PlayerObject::create(1, 1, this, m_objectLayer, false);
		if (f->dummy) {
			f->dummy->retain();
			f->dummy->setVisible(false);
		}

		f->hud = CCLabelBMFont::create("", "chatFont.fnt");
		f->hud->setAnchorPoint({0.f, 1.f});
		auto win = CCDirector::get()->getWinSize();
		f->hud->setPosition({6.f, win.height - 6.f});
		this->addChild(f->hud, 1000);
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		auto& s = vb::S();
		s.frame = 0;
		if (s.recording) {
			s.macro.clear();
			s.idx = 0;
			vb::W() = {};
		}
		if (s.playing) vb::rewindMacro(0);
	}

	void destroyPlayer(PlayerObject* p, GameObject* g) {
		if (vb::S().simulating) {
			if (p) p->m_isDead = true;
			return;
		}
		PlayLayer::destroyPlayer(p, g);
	}

	static void box(CCDrawNode* n, CCRect r, ccColor4F c) {
		CCPoint v[4] = {
			{r.getMinX(), r.getMinY()}, {r.getMaxX(), r.getMinY()},
			{r.getMaxX(), r.getMaxY()}, {r.getMinX(), r.getMaxY()}
		};
		n->drawPolygon(v, 4, ccColor4F{c.r, c.g, c.b, .15f}, .6f, c);
	}

	void drawHitboxes() {
		auto f = m_fields.self();
		float px = m_player1->getPositionX();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			if (std::abs(obj->getPositionX() - px) > 420.f) continue;
			switch (obj->m_objectType) {
				case GameObjectType::Hazard:
				case GameObjectType::AnimatedHazard:
					box(f->draw, obj->getObjectRect(), {1.f, 0.f, 0.f, 1.f});
					break;
				case GameObjectType::Solid:
					box(f->draw, obj->getObjectRect(), {0.f, .5f, 1.f, 1.f});
					break;
				default: break;
			}
		}
		if (m_player1) box(f->draw, m_player1->getObjectRect(), {0.f, 1.f, 0.f, 1.f});
		if (m_player2) box(f->draw, m_player2->getObjectRect(), {0.f, 1.f, 0.f, 1.f});
	}

	void drawTrajectory() {
		auto& s = vb::S();
		auto f = m_fields.self();
		auto d = f->dummy;
		if (!d || !m_player1 || m_player1->m_isDead) return;

		constexpr float stepDt = 1.f / 240.f;
		int steps = std::clamp((int)s.trajSteps, 10, 300);

		for (int mode = 0; mode < 2; mode++) {
			bool hold = (mode == 0);
			auto cp = PlayerCheckpoint::create();
			m_player1->saveToCheckpoint(cp);
			d->loadFromCheckpoint(cp);
			d->m_isDead = false;
			d->setPosition(m_player1->getPosition());
			d->setRotation(m_player1->getRotation());

			std::vector<CCPoint> pts;
			pts.reserve(steps + 1);
			pts.push_back(d->getPosition());

			{
				vb::SimGuard guard(s.simulating);
				for (int i = 0; i < steps; i++) {
					if (hold) d->pushButton(PlayerButton::Jump);
					else d->releaseButton(PlayerButton::Jump);
					d->update(stepDt);
					this->checkCollisions(d, stepDt, false);
					pts.push_back(d->getPosition());
					if (d->m_isDead) break;
				}
			}
			d->releaseButton(PlayerButton::Jump);

			ccColor4F col = hold ? ccColor4F{.2f, 1.f, .3f, 1.f} : ccColor4F{1.f, .2f, .2f, 1.f};
			for (size_t i = 1; i < pts.size(); i++) {
				f->draw->drawSegment(pts[i - 1], pts[i], 1.f, col);
			}
			if (d->m_isDead) {
				auto e = pts.back();
				box(f->draw, CCRect(e.x - 15.f, e.y - 15.f, 30.f, 30.f), {1.f, 0.f, 0.f, 1.f});
			}
		}
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		auto& s = vb::S();
		auto f = m_fields.self();
		if (!f->draw) return;

		f->draw->clear();
		if (!s.disabled && m_player1) {
			if (s.hitboxes) drawHitboxes();
			if (s.trajectory) drawTrajectory();
		}

		if (dt > 0.f) f->fps = f->fps * .9f + (1.f / dt) * .1f;
		std::string txt;
		if (!s.disabled && s.hudFps) {
			txt += fmt::format("FPS {:.0f}  TPS {:.0f}  x{:.2f}\n", f->fps, s.tps, s.speed);
		}
		if (!s.disabled && s.hudMacro) {
			auto const& w = vb::W();
			txt += fmt::format("{}  frame {}  inputs {}  win {}f (min {}f)",
				s.recording ? "REC" : (s.playing ? "PLAY" : "IDLE"),
				s.frame, s.macro.size(), w.lastFrames, w.minFrames);
		}
		f->hud->setVisible(!txt.empty());
		if (!txt.empty()) {
			f->hud->setString(txt.c_str());
			f->hud->setScale(s.hudScale);
		}
	}
};
```[cite: 8]
