// Licensing tests (licensing branch; only built with the spa-licensing module).
// Included from SPAStripTests.cpp before main(). Everything runs against a
// hermetic licensing root (temp dir), a fixed fake machine hash, an injected
// clock and a counting fake transport installed for the WHOLE run in main()
// -- no real network, no real ~/Library licensing files or trial stores. The
// suite's own hermetic presets root applies too.
#if SPASTRIP_HAS_SPA_LICENSING
#include <mutex>

namespace licTests
{
using namespace spa::lic;
using spa::SPAStripEditor;
using spa::ui::ContentComponent;
using PM=spa::preset::PresetManager;

struct FakeTransport : Transport
{
    std::mutex lock;
    std::function<HttpResponse(const juce::String&,const juce::String&)> handler;
    std::atomic<int> calls{0},messageThreadCalls{0};
    juce::String lastUrl,lastBody;

    HttpResponse postJson(const juce::String& url,const juce::String& body,int) override
    {
        ++calls;
        if(auto* mm=juce::MessageManager::getInstanceWithoutCreating())
            if(mm->isThisTheMessageThread()) ++messageThreadCalls;
        std::function<HttpResponse(const juce::String&,const juce::String&)> h;
        { std::lock_guard<std::mutex> g(lock); lastUrl=url; lastBody=body; h=handler; }
        return h ? h(url,body) : HttpResponse{};   // default: "offline"
    }
    void reset()
    {
        std::lock_guard<std::mutex> g(lock);
        handler=nullptr; calls=0; messageThreadCalls=0; lastUrl=lastBody={};
    }
    void setHandler(std::function<HttpResponse(const juce::String&,const juce::String&)> h)
    { std::lock_guard<std::mutex> g(lock); handler=std::move(h); }
    juce::String body() { std::lock_guard<std::mutex> g(lock); return lastBody; }
    juce::String url()  { std::lock_guard<std::mutex> g(lock); return lastUrl; }
};

static std::shared_ptr<FakeTransport>& fake()
{
    static auto f=std::make_shared<FakeTransport>();
    return f;
}

static std::string testMachineHash() { return toHex(sha256(std::string("SPAStripTests machine"))); }

// Committed TEST key kid 1000 (spa-licensing tools/test-keys; not secret,
// trusted only because this target defines SPA_LICENSING_TESTS).
static std::string makeToken(const std::string& serial,const std::string& product="spastrip",
                             const std::string& machine=testMachineHash())
{
    TokenPayload p;
    p.kid=1000; p.product=product; p.serialHash=serialTokenHash(serial); p.machineHash=machine;
    p.issued="2026-10-03"; p.tokenId="0123456789abcdef01234567";
    std::vector<uint8_t> seed;
    fromHex("1b76ccb48c907beba680faf6420b7deaf62a7a8895d3c7fc8a6052545474f1d6",seed);
    return signToken(p,seed.data());
}

static const std::string& testSerial()
{
    static const std::string s=generateSerial("STR");
    return s;
}

static HttpResponse json(int status,const juce::String& body) { return {true,status,body}; }

// Message pump without JUCE_MODAL_LOOPS_PERMITTED (this target, like the
// plugin, is built without it): run the main CFRunLoop, which is where JUCE's
// mac message queue posts, plus any due timers.
static void pump(int ms)
{
    const auto end=juce::Time::getMillisecondCounter()+(juce::uint32)ms;
    do
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    } while(juce::Time::getMillisecondCounter()<end);
}

static bool pumpUntil(const std::function<bool()>& cond,int ms=3000)
{
    const auto deadline=juce::Time::getMillisecondCounter()+(juce::uint32)ms;
    while(!cond())
    {
        if(juce::Time::getMillisecondCounter()>deadline) return false;
        pump(5);
    }
    return true;
}

// Fresh licensing root + presets root + controllable "today" for one test;
// restores the suite-wide hermetic root afterwards.
struct ScopedLicRoot
{
    fs::path saved;
    juce::File dir;
    int64_t today=todayUtcDays();

    ScopedLicRoot()
    {
        saved=licensingRootOverride();
        dir=juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("SPAStripTests-lic-"+juce::String(juce::Random::getSystemRandom().nextInt(1<<30)));
        dir.createDirectory();
        setLicensingRootOverride(pathFromUtf8(dir.getFullPathName().toStdString()));
        setTodayOverride([this]{ return today; });
        fake()->reset();
    }
    ~ScopedLicRoot()
    {
        setTodayOverride({});
        setLicensingRootOverride(saved);
        dir.deleteRecursively();
        fake()->reset();
    }
    juce::File licDir() const { return dir.getChildFile("Licensing"); }
    juce::File tokenFile() const { return licDir().getChildFile("spastrip.lic"); }
    void writeToken(const std::string& t) const
    { expect(writeFileAtomically(pathFromUtf8(tokenFile().getFullPathName().toStdString()),t),"write token"); }
};

// Real hit-test + dispatch: find what the editor's own hit test returns at the
// control's centre, require it to be that control (or inside it), then deliver
// down/up to it.
static void clickControl(juce::Component& editor,juce::Component& control,const char* what)
{
    const auto pos=editor.getLocalArea(&control,control.getLocalBounds()).getCentre().toFloat();
    auto* target=editor.getComponentAt(pos.roundToInt());
    const bool hit=target==&control || (target!=nullptr && control.isParentOf(target));
    expect(hit,juce::String("hit-test reaches ")+what
               +(target!=nullptr ? juce::String(" (got ")+typeid(*target).name()+")" : juce::String(" (got nothing)")));
    auto src=juce::Desktop::getInstance().getMainMouseSource();
    const auto local=target->getLocalPoint(&editor,pos);
    const auto now=juce::Time::getCurrentTime();
    auto make=[&](bool down,int clicks)
    {
        const auto mods=down ? juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys();
        return juce::MouseEvent(src,local,mods,1.0f,0.0f,0.0f,0.0f,0.0f,target,target,now,local,now,clicks,false);
    };
    target->mouseEnter(make(false,0));
    target->mouseDown(make(true,1));
    target->mouseUp(make(false,1));
    pump(15);
}


static juce::TextButton* findTextButton(juce::Component& root,const juce::String& text)
{
    if(auto* b=dynamic_cast<juce::TextButton*>(&root))
        if(b->getButtonText()==text && b->isVisible()) return b;
    for(auto* c:root.getChildren())
        if(auto* f=findTextButton(*c,text)) return f;
    return nullptr;
}

struct Rig
{
    std::unique_ptr<Proc> proc;
    std::unique_ptr<SPAStripEditor> editor;
    ContentComponent* content=nullptr;
    explicit Rig(bool withEditor=true)
    {
        proc=std::make_unique<Proc>();
        proc->noteEditorOpened();   // what a real window open does (off-screen editors have no peer)
        proc->prepareToPlay(48000.0,512);
        if(!withEditor) return;
        editor=std::make_unique<SPAStripEditor>(*proc);
        editor->setVisible(true);
        editor->setSize(spa::ui::metrics::baseWidth,spa::ui::metrics::baseHeight);
        content=&editor->getContent();
        pump(20);
    }
    ~Rig() { editor.reset(); proc.reset(); }
    LicensePanel* panel()
    {
        auto* d=content!=nullptr ? content->getLicenceDialogForTest() : nullptr;
        return d!=nullptr ? &d->getPanel() : nullptr;
    }
    void openPanel() { content->showLicencePanel(); pump(15); }
    void closePanel() { content->dismissDialog(); pump(15); }
};

// `seconds` of a deterministic stereo sine through the default (passthrough) strip.
static std::vector<float> renderSignal(Proc& p,double seconds)
{
    const int block=512;
    const int blocks=(int)std::ceil(seconds*48000.0/block);
    std::vector<float> out; out.reserve((size_t)(blocks*block));
    juce::AudioBuffer<float> buf(2,block);
    juce::MidiBuffer midi;
    int64_t n=0;
    for(int b=0;b<blocks;++b)
    {
        for(int i=0;i<block;++i,++n)
        {
            const float v=0.3f*std::sin((float)n*0.05f);
            buf.setSample(0,i,v); buf.setSample(1,i,v);
        }
        p.processBlock(buf,midi);
        out.insert(out.end(),buf.getReadPointer(0),buf.getReadPointer(0)+block);
    }
    return out;
}

static void licenceTrialStatesTest()
{
    std::cout<<"licenceTrialStatesTest\n";
    ScopedLicRoot root;
    const int64_t day0=root.today;
    Rig rig;
    auto& badge=rig.content->getLicenceBadgeForTest();
    struct Case { int day; int daysLeft; bool demo; const char* text; };
    for(auto c:{Case{0,14,false,"TRIAL: 14 DAYS"},Case{13,1,false,"TRIAL: 1 DAY"},
                Case{14,0,true,"DEMO - ACTIVATE"},Case{15,0,true,"DEMO - ACTIVATE"}})
    {
        root.today=day0+c.day;
        rig.proc->refreshLicence();
        pumpUntil([&]{ return badge.getButtonText()==c.text; },1000);
        const auto tag=" (day "+juce::String(c.day)+")";
        expect(rig.proc->getLicenceState().trialDaysLeft()==c.daysLeft,"trial days left"+tag);
        expect(rig.proc->isDemoActive()==c.demo,"demo flag"+tag);
        expect(badge.getButtonText()==c.text,"badge text "+badge.getButtonText()+tag);
        expect(badge.isVisible(),"badge visible while unlicensed"+tag);
    }
    expect(root.licDir().getChildFile("spastrip.trial").existsAsFile(),"trial store 1 in temp root");
    expect(root.dir.getChildFile("Preferences").getChildFile("com.silverplatteraudio.spastrip.t").existsAsFile(),"trial store 2 in temp root");
    expect(root.dir.getChildFile("AppSupport").getChildFile(".spastrip.t").existsAsFile(),"trial store 3 in temp root");
    root.writeToken(makeToken(testSerial()));
    rig.proc->refreshLicence();
    pumpUntil([&]{ return !badge.isVisible(); },1000);
    expect(rig.proc->getLicenceState().isLicensed() && !rig.proc->isDemoActive(),"licensed after token");
    expect(!badge.isVisible(),"badge hidden when licensed");
    root.writeToken(makeToken(generateSerial("GLT"),"spaglitch"));
    rig.proc->refreshLicence();
    expect(!rig.proc->getLicenceState().isLicensed(),"a SPAGlitch token does not license SPAStrip");
    expect(fake()->calls==0,"no network for trial/state refreshes");
}

static void licenceTrialStartTest()
{
    std::cout<<"licenceTrialStartTest\n";
    ScopedLicRoot root;
    const int64_t day0=root.today;
    auto stores=[&]{ return root.licDir().getChildFile("spastrip.trial").existsAsFile()
                         || root.dir.getChildFile("Preferences").getChildFile("com.silverplatteraudio.spastrip.t").existsAsFile()
                         || root.dir.getChildFile("AppSupport").getChildFile(".spastrip.t").existsAsFile(); };
    {
        Proc p; p.prepareToPlay(48000,512);
        juce::AudioBuffer<float> b(2,512); juce::MidiBuffer m; p.processBlock(b,m);
        auto ed=std::make_unique<SPAStripEditor>(p);
        ed->setSize(spa::ui::metrics::baseWidth,spa::ui::metrics::baseHeight); pump(10);
        expect(!p.getLicenceState().trialStarted(),"no trial start without a window");
        expect(!stores(),"no trial store written by construction/scan/off-screen editor");
        root.today=day0+40;
        p.refreshLicence(); pump(10);
        expect(!p.isDemoActive() && p.getLicenceState().trialDaysLeft()==14,"never demo before the trial starts");
        expect(ed->getContent().getLicenceBadgeForTest().getButtonText()=="TRIAL: 14 DAYS","not-started badge reads as the full trial");
    }
    root.today=day0;
    {
        Proc p;
        auto ed=std::make_unique<SPAStripEditor>(p);
        ed->addToDesktop(0);
        pump(20);
        expect(p.getLicenceState().trialStarted() && stores(),"first window open starts the trial and writes the stores");
        ed->removeFromDesktop();
    }
    root.today=day0+14;
    { Proc p; expect(p.isDemoActive(),"14 days after the first open: demo"); }
}

static void licenceDemoGateTest()
{
    std::cout<<"licenceDemoGateTest\n";
    ScopedLicRoot root;
    const int64_t day0=root.today;
    const double seconds=62.0;
    auto renderWith=[&](bool expectDemo,bool expectLicensed)
    {
        Proc p; p.noteEditorOpened(); p.prepareToPlay(48000.0,512);
        expect(p.isDemoActive()==expectDemo,"demo state for render");
        expect(p.getLicenceState().isLicensed()==expectLicensed,"licensed state for render");
        return renderSignal(p,seconds);
    };
    const auto trial=renderWith(false,false);
    const auto trial2=renderWith(false,false);
    root.writeToken(makeToken(testSerial()));
    const auto licensed=renderWith(false,true);
    root.tokenFile().deleteFile();
    root.today=day0+20;
    const auto demo=renderWith(true,false);

    expect(trial==trial2,"render is deterministic across instances");
    double energy=0; for(auto v:trial) energy+=(double)v*v;
    expect(energy>1.0,"the signal is audible");
    expect(trial==licensed,"trial and licensed output bit-identical (gate not applied)");
    expect(demo!=trial,"demo output differs (gate is really applied)");
    int silent=0,changed=0,firstChanged=-1,lastChanged=-1;
    float maxGain=0,maxStep=0; double prevGain=1;
    for(size_t i=0;i<trial.size();++i)
    {
        if(std::memcmp(&demo[i],&trial[i],sizeof(float))!=0) { ++changed; if(firstChanged<0) firstChanged=(int)i; lastChanged=(int)i; }
        if(std::abs(trial[i])>1.0e-3f)
        {
            const double g=demo[i]/trial[i];
            maxGain=juce::jmax(maxGain,(float)g);
            if(std::abs(demo[i])<1.0e-9f) ++silent;
            maxStep=juce::jmax(maxStep,(float)std::abs(g-prevGain));
            prevGain=g;
        }
    }
    const double firstSec=firstChanged/48000.0,spanSec=(lastChanged-firstChanged)/48000.0;
    std::cout<<"  demo gate: first change "<<firstSec<<" s, span "<<spanSec<<" s, "<<changed
             <<" samples changed, max gain "<<maxGain<<", max gain step "<<maxStep<<"\n";
    expect(firstSec>29.9 && firstSec<30.1,"first mute ~30 s into demo playback");
    expect(spanSec>1.4 && spanSec<1.51,"one ~1.5 s mute in 62 s");
    expect(changed<(int)(1.6*48000),"only the mute window changed");
    expect(maxGain<=1.0001f,"the gate never boosts");
    expect(maxStep<0.01f,"fades are smooth (no click)");
    expect(silent>0,"the mute is real silence");
    // Host bypass is never gated (it is the dry signal).
    {
        Proc p; p.prepareToPlay(48000.0,512);
        juce::AudioBuffer<float> buf(2,512); juce::MidiBuffer midi;
        bool intact=true;
        for(int b=0;b<(int)(62*48000/512);++b)
        {
            for(int i=0;i<512;++i) { buf.setSample(0,i,0.25f); buf.setSample(1,i,0.25f); }
            p.processBlockBypassed(buf,midi);
            if(b>(int)(1.0*48000/512) && buf.getMagnitude(0,0,512)<0.2f) intact=false;
        }
        expect(intact,"demo: host bypass output never muted");
    }
    expect(fake()->calls==0,"no network from the audio path");
}

static void licencePresetSaveBlockedTest()
{
    std::cout<<"licencePresetSaveBlockedTest\n";
    ScopedLicRoot root;
    const int64_t day0=root.today;
    Rig rig;
    auto& pm=rig.proc->getPresetManager();
    const auto out=root.dir.getChildFile("export"); out.createDirectory();

    auto saved=pm.save("Lic Trial Save",{});
    expect(saved.ok && saved.file.existsAsFile(),"trial: save writes");
    pm.rescan();
    const PM::PresetInfo* info=nullptr;
    for(const auto& p:pm.getPresets()) if(p.file==saved.file) info=&p;
    expect(info!=nullptr,"saved preset scanned");
    if(info==nullptr) return;
    const auto infoCopy=*info;
    expect(pm.exportPreset(infoCopy,out.getChildFile("a.spastrip")),"trial: export works");
    expect(pm.saveInPlace(saved.file).ok,"trial: saveInPlace works");

    root.today=day0+30;
    rig.proc->refreshLicence();
    expect(rig.proc->isDemoActive(),"demo active");
    int blockedCalls=0;
    auto editorHook=pm.onSaveBlocked;
    pm.onSaveBlocked=[&]{ ++blockedCalls; };
    auto r=pm.save("Lic Demo Save",{});
    expect(!r.ok && !pm.getUserFolder().getChildFile("Lic Demo Save.spastrip").exists(),"demo: save refused, nothing written");
    const auto before=saved.file.loadFileAsString();
    setParam(*rig.proc,pid::outputGain,-6.0f);
    expect(!pm.saveInPlace(saved.file).ok && saved.file.loadFileAsString()==before,"demo: saveInPlace refused, file untouched");
    expect(!pm.exportPreset(infoCopy,out.getChildFile("b.spastrip")) && !out.getChildFile("b.spastrip").exists(),"demo: exportPreset refused");
    expect(!pm.exportFolder("",out.getChildFile("c.zip")) && !out.getChildFile("c.zip").exists(),"demo: exportFolder refused");
    expect(blockedCalls==4,"onSaveBlocked fired for each refusal ("+juce::String(blockedCalls)+")");
    pm.onSaveBlocked=editorHook;

    // Loading presets and host sessions are never affected.
    pm.rescan();
    int idx=-1;
    for(int i=0;i<(int)pm.getPresets().size();++i) if(pm.getPresets()[(size_t)i].file==saved.file) idx=i;
    expect(idx>=0 && pm.loadPreset(idx).ok,"demo: loading a preset works");
    juce::MemoryBlock session; rig.proc->getStateInformation(session);
    expect(session.getSize()>0,"demo: host session save works");
    {
        Proc p2;
        expect(p2.isDemoActive(),"second instance also in demo");
        setParam(*rig.proc,pid::outputGain,-4.0f);
        rig.proc->getStateInformation(session);
        p2.setStateInformation(session.getData(),(int)session.getSize());
        expect(std::abs(getParam(p2,pid::outputGain)+4.0f)<0.01f,"demo: host session restore works");
    }

    // SAVE (real click) in demo: no save menu/dialog; the licence panel opens with the reason.
    auto* saveBtn=findTextButton(*rig.editor,"SAVE");
    expect(saveBtn!=nullptr,"SAVE button found");
    if(saveBtn==nullptr) return;
    clickControl(*rig.editor,*saveBtn,"SAVE (demo)");
    pumpUntil([&]{ return rig.panel()!=nullptr; },500);
    expect(rig.panel()!=nullptr,"demo: SAVE opens the licence panel");
    if(rig.panel()!=nullptr)
    {
        expect(rig.panel()->getStatusText()=="Demo mode","panel shows demo state");
        clickControl(*rig.editor,rig.panel()->getCloseButton(),"licence panel Close");
        pumpUntil([&]{ return rig.content->getDialogForTest()==nullptr; },500);
        expect(rig.content->getDialogForTest()==nullptr,"Close closes the licence panel");
    }
    // Anti-vacuous: in trial the same click (no user preset current -> Save As) opens the save dialog.
    root.today=day0;
    rig.proc->refreshLicence();
    // With the current preset's file gone SAVE goes straight to the Save As
    // dialog (a menu can't open on an off-screen test editor).
    saved.file.deleteFile();
    clickControl(*rig.editor,*saveBtn,"SAVE (trial)");
    pump(30);
    const bool panelShown=rig.panel()!=nullptr;
    const bool dialogShown=rig.content->getDialogForTest()!=nullptr;
    const bool menuShown=juce::PopupMenu::dismissAllActiveMenus();
    expect((menuShown || dialogShown) && !panelShown,"trial: SAVE opens the Save As dialog, not the licence panel (menu "
           +juce::String((int)menuShown)+", dialog "+juce::String((int)dialogShown)+", panel "+juce::String((int)panelShown)+")");
    rig.closePanel();
    expect(fake()->calls==0,"no network while saving/loading");
}

static void licencePanelOnlineActivationTest()
{
    std::cout<<"licencePanelOnlineActivationTest\n";
    ScopedLicRoot root;
    Rig rig;
    auto& badge=rig.content->getLicenceBadgeForTest();
    expect(badge.isVisible() && badge.getButtonText()=="TRIAL: 14 DAYS","trial badge before activation");
    clickControl(*rig.editor,badge,"licence badge");
    pumpUntil([&]{ return rig.panel()!=nullptr; },500);
    auto* panel=rig.panel();
    expect(panel!=nullptr,"badge opens the licence panel");
    if(panel==nullptr) return;
    expect(panel->getStatusText()=="Trial: 14 days left","panel trial status: "+panel->getStatusText());

    auto bad=juce::String(testSerial());
    bad=bad.substring(0,bad.length()-1)+(bad.getLastCharacter()=='A' ? "B" : "A");
    panel->getSerialEditor().setText(bad,juce::sendNotification); pump(20);
    expect(panel->getSerialHint().isNotEmpty(),"live check flags a typo");
    clickControl(*rig.editor,panel->getActivateButton(),"Activate (bad serial)");
    expect(fake()->calls==0,"invalid serial never reaches the network");

    fake()->setHandler([](const juce::String&,const juce::String&)
    {
        return json(409,"{\"error\":\"seat_limit\",\"message\":\"This serial is active on 3 of 3 computers.\",\"seatsTotal\":3,"
                        "\"activations\":[{\"machineName\":\"Studio iMac\",\"activatedAt\":\"2026-09-01\"},"
                        "{\"machineName\":\"Road Laptop\",\"activatedAt\":\"2026-09-02\"},"
                        "{\"machineName\":\"Home PC\",\"activatedAt\":\"2026-09-03\"}]}");
    });
    panel->getSerialEditor().setText(juce::String(testSerial()).toLowerCase().replace("-"," "),juce::sendNotification); pump(20);
    expect(panel->getSerialHint()=="Serial number looks good.","live check accepts a lower-case, spaced serial");
    panel->getMachineEditor().setText("Mike's Studio",juce::sendNotification); pump(20);
    clickControl(*rig.editor,panel->getActivateButton(),"Activate (seat limit)");
    pumpUntil([&]{ return !rig.proc->getLicenceController().busy(); });
    expect(fake()->calls==1 && fake()->messageThreadCalls==0,"one request, off the message thread");
    expect(panel->getMessageText().contains("Studio iMac"),"seat_limit shows the machine list");

    const auto token=makeToken(testSerial());
    fake()->setHandler([token](const juce::String&,const juce::String&)
                       { return json(200,"{\"token\":\""+juce::String(token)+"\"}"); });
    clickControl(*rig.editor,panel->getActivateButton(),"Activate (success)");
    pumpUntil([&]{ return rig.proc->getLicenceState().isLicensed(); });
    expect(rig.proc->getLicenceState().isLicensed() && root.tokenFile().existsAsFile(),"activation installs the token");
    const auto body=juce::JSON::parse(fake()->body());
    expect(body["product"].toString()=="spastrip" && body["appVersion"].toString()==SPASTRIP_VERSION
           && body["machineHash"].toString()==juce::String(testMachineHash()),"product/version/machine sent");
    pumpUntil([&]{ return !badge.isVisible(); },500);
    expect(!badge.isVisible() && panel->getStatusText()=="Activated","badge hidden, panel Activated");

    fake()->setHandler([](const juce::String&,const juce::String&){ return json(200,"{\"ok\":true}"); });
    clickControl(*rig.editor,panel->getDeactivateButton(),"Deactivate (online)");
    pumpUntil([&]{ return !rig.proc->getLicenceState().isLicensed(); });
    expect(!root.tokenFile().exists() && fake()->url().endsWith("/v1/deactivate"),"deactivate removes the token");
}

static void licenceOfflineActivationTest()
{
    std::cout<<"licenceOfflineActivationTest\n";
    ScopedLicRoot root;
    Rig rig;
    rig.openPanel();
    auto* panel=rig.panel();
    expect(panel!=nullptr,"panel open");
    if(panel==nullptr) return;
    panel->getSerialEditor().setText(juce::String(testSerial()),juce::sendNotification); pump(20);
    panel->getMachineEditor().setText("Offline Box",juce::sendNotification); pump(20);
    clickControl(*rig.editor,panel->getOfflineButton(),"Activate offline...");
    expect(panel->getPage()==LicensePanel::Page::offline,"offline page shown");
    juce::var challenge;
    expect(parseOfflineChallenge(panel->getChallengeText(),challenge) && challenge["product"].toString()=="spastrip"
           && challenge["machineName"].toString()=="Offline Box","challenge carries product + machine");
    panel->getResponseEditor().setText(juce::String(makeToken(generateSerial("SYN"),"spasynth")),false);
    clickControl(*rig.editor,panel->getSubmitOfflineButton(),"offline Activate (wrong product)");
    expect(!rig.proc->getLicenceState().isLicensed(),"other product's token refused");
    panel->getResponseEditor().setText("Your SPAStrip activation:\n"+juce::String(makeToken(testSerial()))+"\n",false);
    clickControl(*rig.editor,panel->getSubmitOfflineButton(),"offline Activate");
    expect(rig.proc->getLicenceState().isLicensed(),"pasted token activates");
    expect(panel->getStatusText()=="Activated","panel back to Activated");
    expect(fake()->calls==0,"offline activation makes no network call");
}

static void licenceNoNetworkOnConstructionTest()
{
    std::cout<<"licenceNoNetworkOnConstructionTest\n";
    ScopedLicRoot root;
    {
        Rig rig;
        juce::AudioBuffer<float> buf(2,512); juce::MidiBuffer midi;
        for(int i=0;i<200;++i) rig.proc->processBlock(buf,midi);
        rig.openPanel();
        if(auto* p=rig.panel()) { p->getSerialEditor().setText(juce::String(testSerial()),juce::sendNotification); pump(20); }
        rig.closePanel();
        rig.proc->refreshLicence();
        pump(100);
    }
    { Proc p; p.prepareToPlay(44100,256); }
    expect(fake()->calls==0,"no network on construction, refresh, audio, panel open or typing");
}

static void licenceBadgeLayoutAndAboutTest()
{
    std::cout<<"licenceBadgeLayoutAndAboutTest\n";
    ScopedLicRoot root;
    Rig rig;
    auto& badge=rig.content->getLicenceBadgeForTest();
    expect(badge.isVisible(),"badge visible in trial");
    const auto b=badge.getBounds();
    expect(b.getY()>=0 && b.getBottom()<=spa::ui::metrics::brandBandHeight,"badge inside the brand band");
    expect(b.getRight()<=spa::ui::metrics::baseWidth-8,"badge clear of the right edge");
    expect(b.getX()>spa::ui::metrics::baseWidth/2+200,"badge clear of the wordmark");
    for(auto* c:rig.content->getChildren())
        if(c!=&badge && c->isVisible() && c->getBounds().intersects(b))
            expect(false,juce::String("badge overlaps ")+typeid(*c).name()+" "+c->getBounds().toString());
    clickControl(*rig.editor,badge,"badge");
    pumpUntil([&]{ return rig.panel()!=nullptr; },500);
    expect(rig.panel()!=nullptr,"badge click opens panel");
    rig.closePanel();

    // Logo menu: real hit test reaches the logo; its menu has Licence... (id 3).
    auto& logo=rig.content->getLogoButtonForTest();
    const auto at=rig.editor->getLocalArea(&logo,logo.getLocalBounds()).getCentre();
    auto* hit=rig.editor->getComponentAt(at);
    expect(hit==&logo || (hit!=nullptr && logo.isParentOf(hit)),"hit-test reaches the logo button");
    auto menu=rig.content->createLogoMenuForTest();
    juce::PopupMenu::MenuItemIterator it(menu);
    bool found=false;
    while(it.next()) if(it.getItem().text=="Licence..." && it.getItem().itemID==3) found=true;
    expect(found,"logo menu has Licence...");

    // About shows the licence state and grows a row for it.
    rig.content->showAboutPanel(); pump(15);
    auto* about=dynamic_cast<spa::ui::AboutDialog*>(rig.content->getDialogForTest());
    expect(about!=nullptr,"About open");
    if(about!=nullptr)
    {
        expect(about->getLicenceLine()=="Licence: trial, 14 days left","About licence line: "+about->getLicenceLine());
        expect(about->getInfoTextForTest().contains("Licence: trial, 14 days left"),"About info shows it");
        expect(about->buildClipboardText().contains("Licence: trial"),"Copy Info includes it");
        expect(about->getCardSize().y==516,"About card grew one row");
    }
    rig.closePanel();
    root.writeToken(makeToken(testSerial()));
    rig.proc->refreshLicence();
    pumpUntil([&]{ return !badge.isVisible(); },500);
    expect(!badge.isVisible(),"no badge when licensed");
}

static void renderLicenceUi(const juce::File& dir)
{
    dir.createDirectory();
    auto savePng=[&](const juce::Image& img,const juce::String& name)
    {
        juce::FileOutputStream os(dir.getChildFile(name)); os.setPosition(0); os.truncate();
        juce::PNGImageFormat().writeImageToStream(img,os);
        std::cout<<"  wrote "<<dir.getChildFile(name).getFullPathName()<<"\n";
    };
    auto snapPanel=[&](Rig& rig,const juce::String& name)
    { if(auto* p=rig.panel()) savePng(p->createComponentSnapshot(p->getLocalBounds(),true,2.0f),name); };
    auto snapBand=[&](Rig& rig,const juce::String& name)
    { savePng(rig.editor->createComponentSnapshot({0,0,spa::ui::metrics::baseWidth,90},true,2.0f),name); };
    ScopedLicRoot root;
    const int64_t day0=root.today;
    {
        root.today=day0+5; Rig rig; rig.proc->refreshLicence(); pump(20);
        snapBand(rig,"badge-trial.png");
        rig.openPanel(); snapPanel(rig,"panel-trial.png");
        savePng(rig.editor->createComponentSnapshot(rig.editor->getLocalBounds(),true,1.0f),"editor-with-panel.png");
        rig.closePanel(); rig.content->showAboutPanel(); pump(15);
        savePng(rig.editor->createComponentSnapshot(rig.editor->getLocalBounds(),true,1.0f),"about-with-licence.png");
    }
    {
        root.today=day0+30; Rig rig; rig.proc->refreshLicence(); pump(20);
        snapBand(rig,"badge-demo.png");
        rig.content->showLicencePanel("Saving and exporting presets is off in demo mode. Activate to save your settings.");
        pump(20); snapPanel(rig,"panel-demo-save-blocked.png");
        rig.panel()->getOfflineButton().triggerClick(); pump(20);
        snapPanel(rig,"panel-offline.png");
    }
    {
        root.today=day0; Rig rig;
        rig.proc->getLicenceController().importOfflineResponse(juce::String(makeToken(testSerial())),juce::String(testSerial()),"Studio iMac");
        pump(20); snapBand(rig,"badge-licensed-none.png");
        rig.openPanel(); snapPanel(rig,"panel-activated.png");
    }
}
} // namespace licTests

// Suite-wide licensing hermeticity, set before ANY processor exists: a temp
// licensing root (token + all three trial stores), a fixed fake machine hash
// and the counting fake transport. The real files are fingerprinted now and
// re-checked at exit ("TEST LEAK" + exit 1 if any changed).
struct LicensingHermeticRun
{
    juce::File dir;
    juce::StringArray before;

    static juce::StringArray fingerprint()
    {
        juce::StringArray out;
        const auto home=juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        const juce::File files[]{
            juce::File(juce::String(spa::lic::pathToUtf8(spa::lic::userLicensingDir()))),
            home.getChildFile("Library/Preferences/com.silverplatteraudio.spastrip.t"),
            home.getChildFile("Library/Application Support/Silverplatter Audio/.spastrip.t")};
        for(const auto& f:files)
        {
            out.add(f.getFullPathName()+(f.exists() ? " exists "+juce::String(f.getLastModificationTime().toMilliseconds()) : " absent"));
            if(f.isDirectory())
                for(const auto& c:f.findChildFiles(juce::File::findFiles,false))
                    out.add(c.getFileName()+" "+juce::String(c.getLastModificationTime().toMilliseconds())+" "+juce::String(c.getSize()));
        }
        return out;
    }

    LicensingHermeticRun()
    {
        before=fingerprint();   // before the override: the REAL locations
        dir=juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("SPAStripTests-licensing-"+juce::String((juce::int64)juce::Time::getMillisecondCounterHiRes())
                              +"-"+juce::String(juce::Random::getSystemRandom().nextInt(1000000)));
        dir.createDirectory();
        spa::lic::setLicensingRootOverride(spa::lic::pathFromUtf8(dir.getFullPathName().toStdString()));
        spa::lic::setMachineHashOverride(licTests::testMachineHash());
        spa::lic::setTransportOverrideForTests(licTests::fake());
    }
    ~LicensingHermeticRun()
    {
        spa::lic::setLicensingRootOverride({});
        spa::lic::setMachineHashOverride({});
        dir.deleteRecursively();
        const auto after=fingerprint();
        if(after!=before)
        {
            std::cout<<"\n!!!! TEST LEAK !!!! the REAL licensing folder / trial stores changed during this run:\n  before: "
                     <<before.joinIntoString(" | ")<<"\n  after:  "<<after.joinIntoString(" | ")<<"\n"<<std::flush;
            std::_Exit(1);
        }
    }
};

#endif
