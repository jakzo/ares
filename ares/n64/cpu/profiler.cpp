namespace {

auto readBigEndian16(const std::vector<u8>& data, size_t offset) -> u16 {
  if(offset + 2 > data.size()) return 0;
  return u16(data[offset]) << 8 | u16(data[offset + 1]);
}

auto readBigEndian32(const std::vector<u8>& data, size_t offset) -> u32 {
  if(offset + 4 > data.size()) return 0;
  return u32(data[offset]) << 24 | u32(data[offset + 1]) << 16
       | u32(data[offset + 2]) << 8 | u32(data[offset + 3]);
}

auto symbolName(const std::vector<u8>& data, size_t offset, size_t limit) -> std::string {
  std::string result;
  while(offset < limit && offset < data.size() && data[offset]) {
    result += char(data[offset++]);
  }
  return result;
}

}

auto CPU::Profiler::power(bool) -> void {
  if(active) endCapture(cycles());
  isConfigured = false;
  shutdownRequested = false;
  pendingLevelStart = false;
  replayActive = false;
  replayRequested = false;
  gameFrameActive = false;
  functions.clear();
  functionStarts.clear();
  functionCache.clear();
  stageLoadFunction = NoFunction;
  stageUnloadFunction = NoFunction;
  replayStageLoadFunction = NoFunction;
  replayStopFunction = NoFunction;
  masterDisplayListFunction = NoFunction;
  debugMenuDrawFunction = NoFunction;
  softwareTlbLoadFunction = NoFunction;

  auto symbols = std::getenv("ARES_N64_PROFILE_SYMBOLS");
  if(!symbols || !*symbols) return;
  symbolsPath = symbols;

  auto output = std::getenv("ARES_N64_PROFILE_OUTPUT");
  outputPrefix = output && *output ? output : "ares-n64-profile";
  auto replay = std::getenv("ARES_N64_PROFILE_REPLAY");
  replayRequested = replay && *replay && std::string_view(replay) != "0";

  if(!loadSymbols(symbolsPath)) {
    std::fprintf(stderr, "ares N64 profiler: could not load ELF symbols from %s\n", symbolsPath.c_str());
    return;
  }

  for(size_t index = 0; index < functions.size(); index++) {
    if(functions[index].name == "lvlStageLoad") stageLoadFunction = index;
    if(functions[index].name == "lvlUnloadStageTextData") stageUnloadFunction = index;
    if(functions[index].name == "practice_replay_on_stage_load") replayStageLoadFunction = index;
    if(functions[index].name == "practice_replay_stop_playback") replayStopFunction = index;
    if(functions[index].name == "dynGetMasterDisplayList") masterDisplayListFunction = index;
    if(functions[index].name == "debmenuDraw") debugMenuDrawFunction = index;
    if(functions[index].name == "tlbmanageTranslateLoadRomFromTlbAddress") softwareTlbLoadFunction = index;
  }
  if(stageLoadFunction == NoFunction || stageUnloadFunction == NoFunction) {
    std::fprintf(stderr, "ares N64 profiler: ELF is missing lvlStageLoad or lvlUnloadStageTextData\n");
    return;
  }

  functionCache.resize(FunctionCacheSize);
  isConfigured = true;
  configuredAt = std::chrono::steady_clock::now();
  std::fprintf(stderr, "ares N64 profiler: loaded %zu functions from %s\n",
               functions.size(), symbolsPath.c_str());
}

auto CPU::Profiler::unload() -> void {
  if(active) endCapture(cycles());
  isConfigured = false;
}

auto CPU::Profiler::loadSymbols(const std::string& path) -> bool {
  std::ifstream input(path, std::ios::binary);
  if(!input) return false;
  std::vector<u8> data((std::istreambuf_iterator<char>(input)), {});
  if(data.size() < 52) return false;
  if(data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return false;
  if(data[4] != 1 || data[5] != 2) return false;  //ELF32, big-endian

  auto sectionOffset = readBigEndian32(data, 32);
  auto sectionEntrySize = readBigEndian16(data, 46);
  auto sectionCount = readBigEndian16(data, 48);
  if(sectionEntrySize < 40 || sectionOffset + u64(sectionEntrySize) * sectionCount > data.size()) return false;

  for(u32 sectionIndex = 0; sectionIndex < sectionCount; sectionIndex++) {
    auto section = sectionOffset + size_t(sectionIndex) * sectionEntrySize;
    if(readBigEndian32(data, section + 4) != 2) continue;  //SHT_SYMTAB
    auto symbolsOffset = readBigEndian32(data, section + 16);
    auto symbolsSize = readBigEndian32(data, section + 20);
    auto stringsIndex = readBigEndian32(data, section + 24);
    auto symbolSize = readBigEndian32(data, section + 36);
    if(symbolSize < 16 || stringsIndex >= sectionCount) continue;

    auto stringsSection = sectionOffset + size_t(stringsIndex) * sectionEntrySize;
    auto stringsOffset = readBigEndian32(data, stringsSection + 16);
    auto stringsSize = readBigEndian32(data, stringsSection + 20);
    if(u64(symbolsOffset) + symbolsSize > data.size()) continue;
    if(u64(stringsOffset) + stringsSize > data.size()) continue;

    for(size_t symbol = symbolsOffset; symbol + symbolSize <= u64(symbolsOffset) + symbolsSize; symbol += symbolSize) {
      auto nameOffset = readBigEndian32(data, symbol + 0);
      auto address = readBigEndian32(data, symbol + 4);
      auto size = readBigEndian32(data, symbol + 8);
      auto info = data[symbol + 12];
      if((info & 15) != 2 || !size || nameOffset >= stringsSize) continue;  //STT_FUNC
      auto name = symbolName(data, stringsOffset + nameOffset, stringsOffset + stringsSize);
      if(name.empty() || name[0] == '$' || name.rfind(".L", 0) == 0) continue;
      functions.push_back({address, size, std::move(name)});
    }
  }

  std::sort(functions.begin(), functions.end(), [](const Function& a, const Function& b) {
    if(a.address != b.address) return a.address < b.address;
    if(a.size != b.size) return a.size > b.size;
    return a.name < b.name;
  });
  functions.erase(std::unique(functions.begin(), functions.end(), [](const Function& a, const Function& b) {
    return a.address == b.address && a.name == b.name;
  }), functions.end());
  for(size_t index = 0; index < functions.size(); index++) {
    functionStarts.try_emplace(functions[index].address, index);
  }
  return !functions.empty();
}

auto CPU::Profiler::functionAt(u32 address) -> size_t {
  auto& cached = functionCache[address >> 2 & (FunctionCacheSize - 1)];
  if(cached.address == address && cached.function != ~u32{0}) {
    return cached.function == ~u32{1} ? NoFunction : cached.function;
  }

  size_t result = NoFunction;
  if(functions.empty()) return NoFunction;
  auto iterator = std::upper_bound(functions.begin(), functions.end(), address,
    [](u32 address, const Function& function) { return address < function.address; });
  while(iterator != functions.begin()) {
    --iterator;
    if(address >= iterator->address && u64(address) < u64(iterator->address) + iterator->size) {
      result = size_t(iterator - functions.begin());
      break;
    }
    if(iterator->address != address) break;
  }
  cached.address = address;
  cached.function = result == NoFunction ? ~u32{1} : u32(result);
  return result;
}

auto CPU::Profiler::functionStartingAt(u32 address) const -> size_t {
  auto found = functionStarts.find(address);
  return found == functionStarts.end() ? NoFunction : found->second;
}

auto CPU::Profiler::cycles() const -> u64 {
  auto pending = self.clock > 0 ? u64(self.clock >> 1) : 0;
  return u64(self.profile.cpuCycles) + pending;
}

auto CPU::Profiler::resetCapture() -> void {
  for(auto& function : functions) {
    function.calls = 0;
    function.selfCycles = 0;
    function.inclusiveCycles = 0;
  }
  stack.clear();
  pages.clear();
  frames.clear();
  gameFrames.clear();
  folded.clear();
  foldedKey.clear();
  foldedPendingCycles = 0;
  tlbCacheHits = 0;
  tlbCacheMisses = 0;
  tlbMissing = 0;
  lastFunction = NoFunction;
  frameStartCycle = 0;
  gameFrameStartCycle = 0;
  gameFrameTlbLoads = 0;
  gameFrameActive = false;
  replayActive = replayRequested;
  pendingReplayStartReturn = 0;
  pendingCall = false;
}

auto CPU::Profiler::beginCapture(u32 stage, u64 now) -> void {
  resetCapture();
  active = true;
  captureStage = stage;
  captureStartCycle = captureEndCycle = lastCycle = now;
  captureStartedAt = std::chrono::steady_clock::now();
  captureSequence++;
  std::fprintf(stderr, "ares N64 profiler: capture %u started for stage %u\n",
               captureSequence, captureStage);
}

auto CPU::Profiler::endCapture(u64 now, bool requestShutdownAfterWrite) -> void {
  if(!active) return;
  attributeUntil(now);
  flushFolded();
  frameStartCycle = 0;
  while(!stack.empty()) popFunction(now);
  captureEndCycle = now;
  active = false;
  writeCapture();
  if(requestShutdownAfterWrite) requestShutdown();
}

auto CPU::Profiler::checkTimeout(u64 now) -> bool {
  using namespace std::chrono_literals;
  auto elapsed = std::chrono::steady_clock::now() - (active ? captureStartedAt : configuredAt);
  if(!active && elapsed >= 60s) {
    std::fprintf(stderr, "ares N64 profiler: timed out waiting 60 seconds for capture to start\n");
    requestShutdown();
    return true;
  }
  if(active && elapsed >= 30min) {
    std::fprintf(stderr, "ares N64 profiler: timed out waiting 30 minutes for capture to finish; writing partial capture\n");
    endCapture(now, true);
    return true;
  }
  return false;
}

auto CPU::Profiler::requestShutdown() -> void {
  if(shutdownRequested) return;
  shutdownRequested = true;
  if(platform) platform->event(ares::Event::Shutdown);
}

auto CPU::Profiler::attributeUntil(u64 now) -> void {
  if(!active || now < lastCycle) {
    lastCycle = now;
    return;
  }
  auto delta = now - lastCycle;
  if(lastFunction != NoFunction) functions[lastFunction].selfCycles += delta;
  foldedPendingCycles += delta;
  lastCycle = now;
}

auto CPU::Profiler::flushFolded() -> void {
  if(foldedPendingCycles && !foldedKey.empty()) folded[foldedKey] += foldedPendingCycles;
  foldedPendingCycles = 0;
}

auto CPU::Profiler::updateFoldedKey() -> void {
  foldedKey.clear();
  for(auto& entry : stack) {
    if(!foldedKey.empty()) foldedKey += ';';
    foldedKey += functions[entry.function].name;
  }
  if(foldedKey.empty() && lastFunction != NoFunction) foldedKey = functions[lastFunction].name;
}

auto CPU::Profiler::pushFunction(size_t function, u32 returnAddress, u64 now) -> void {
  if(function == NoFunction) return;

  // If a call site is reached again while its previous frame is still on the
  // recovered stack, a return boundary was missed (usually across a JIT or
  // scheduler transition). Discard that stale frame and anything above it.
  for(size_t depth = stack.size(); depth; depth--) {
    auto& entry = stack[depth - 1];
    if(entry.function != function || entry.returnAddress != returnAddress) continue;
    while(stack.size() >= depth) popFunction(now);
    break;
  }

  flushFolded();
  stack.push_back({function, returnAddress, u32(self.ipu.r[29].u64), now});
  functions[function].calls++;
  updateFoldedKey();
}

auto CPU::Profiler::popFunction(u64 now) -> void {
  if(stack.empty()) return;
  flushFolded();
  auto entry = stack.back();
  stack.pop_back();
  if(now >= entry.startCycle) functions[entry.function].inclusiveCycles += now - entry.startCycle;
  updateFoldedKey();
}

auto CPU::Profiler::instruction(u64 address_, u32 instruction_) -> void {
  if(!isConfigured) return;
  auto address = u32(address_);
  auto now = cycles();
  auto currentFunction = functionAt(address);
  auto exactFunction = functionStartingAt(address);
  auto inFunction = [&](size_t function) {
    if(function == NoFunction) return false;
    auto& symbol = functions[function];
    return address >= symbol.address && u64(address) < u64(symbol.address) + symbol.size;
  };
  auto inReplayStageLoad = inFunction(replayStageLoadFunction);
  auto inReplayStop = inFunction(replayStopFunction);

  if(pendingLevelStart && address == pendingLevelReturn) {
    pendingLevelStart = false;
    beginCapture(pendingLevelStage, now);
  }

  if(exactFunction == stageLoadFunction) {
    pendingLevelStage = u32(self.ipu.r[4].u64);
    if(active) endCapture(now, pendingLevelStage == GoldenEyeTitleStage);
    pendingLevelReturn = u32(self.ipu.r[31].u64);
    pendingLevelStart = pendingLevelStage != GoldenEyeTitleStage;
    if(shutdownRequested) return;
  }
  // Start at lvlStageLoad's return instruction. Waiting only for the caller's
  // return address is unreliable when execution crosses JIT/scheduler blocks.
  if(pendingLevelStart && currentFunction == stageLoadFunction && instruction_ == 0x03e0'0008u) {
    pendingLevelStart = false;
    beginCapture(pendingLevelStage, now);
  }
  if(exactFunction == stageUnloadFunction && active) {
    endCapture(now, true);
    return;
  }
  if(replayRequested && !active && inReplayStageLoad) {
    beginCapture(0, now);
  }
  if(replayRequested && active && inReplayStop) {
    endCapture(now, true);
    return;
  }
  if(!active) return;

  if(pendingReplayStartReturn && address == pendingReplayStartReturn) {
    pendingReplayStartReturn = 0;
    replayActive = true;
  }
  if(inReplayStageLoad && !pendingReplayStartReturn) {
    pendingReplayStartReturn = u32(self.ipu.r[31].u64);
  }
  if(inReplayStop) {
    replayActive = false;
    gameFrameActive = false;
  }
  // The JIT can skip the caller's return PC, but it consistently exposes each
  // callee's JR RA. These bracket the same work as the guest-side profiler:
  // after dynGetMasterDisplayList through the end of debmenuDraw.
  if(replayActive && currentFunction == masterDisplayListFunction
      && instruction_ == 0x03e0'0008u) {
    gameFrameActive = true;
    gameFrameStartCycle = now;
    gameFrameTlbLoads = 0;
  }
  if(gameFrameActive && currentFunction == debugMenuDrawFunction
      && instruction_ == 0x03e0'0008u && now >= gameFrameStartCycle) {
    gameFrames.push_back({gameFrameStartCycle, now, gameFrameTlbLoads});
    gameFrameActive = false;
  }
  if(exactFunction == softwareTlbLoadFunction && gameFrameActive) {
    gameFrameTlbLoads++;
  }

  attributeUntil(now);

  auto stackPointer = u32(self.ipu.r[29].u64);

  // The MIPS stack grows downward. If execution is back outside the current
  // function with SP restored to (or above) its entry value, its return was
  // crossed even if the exact return PC was hidden by a JIT/scheduler edge.
  // Do not apply this while a call and its delay slot are still pending: a
  // callee initially sees the caller's SP before running its own prologue.
  if(!pendingCall) {
    while(!stack.empty() && currentFunction != stack.back().function
        && stackPointer >= stack.back().stackPointer) {
      popFunction(now);
    }
  }

  while(!stack.empty() && stack.back().returnAddress && stack.back().returnAddress == address) {
    popFunction(now);
  }

  bool enteredCall = false;
  if(pendingCall && address != pendingCallDelay) {
    if(address == pendingCallTarget) {
      pushFunction(currentFunction, pendingCallReturn, now);
      enteredCall = true;
    }
    pendingCall = false;
  }

  if(stack.empty()) {
    if(currentFunction != NoFunction) pushFunction(currentFunction, 0, now);
  } else if(!enteredCall && exactFunction != NoFunction && stack.back().function != exactFunction) {
    // Recover calls that crossed an unobserved JAL/JALR edge using SP and RA.
    // A restored/higher SP instead indicates a tail call or context switch.
    if(stackPointer < stack.back().stackPointer) {
      pushFunction(exactFunction, u32(self.ipu.r[31].u64), now);
    } else {
      while(!stack.empty()) popFunction(now);
      pushFunction(exactFunction, 0, now);
    }
  }

  lastFunction = currentFunction;

  auto opcode = instruction_ >> 26;
  if(opcode == 3) {  //JAL
    pendingCall = true;
    pendingCallDelay = address + 4;
    pendingCallTarget = (address & 0xf000'0000u) | (instruction_ & 0x03ff'ffffu) << 2;
    pendingCallReturn = address + 8;
  } else if(opcode == 0 && (instruction_ & 63) == 9) {  //JALR
    auto source = instruction_ >> 21 & 31;
    pendingCall = true;
    pendingCallDelay = address + 4;
    pendingCallTarget = u32(self.ipu.r[source].u64);
    pendingCallReturn = address + 8;
  }
}

auto CPU::Profiler::frame() -> void {
  if(isConfigured && checkTimeout(cycles())) return;
  if(!active) return;
  auto now = cycles();
  attributeUntil(now);
  if(frameStartCycle && now >= frameStartCycle) frames.push_back({frameStartCycle, now});
  frameStartCycle = now;
}

auto CPU::Profiler::tlbAccess(u64 address, bool store, TlbResult result) -> void {
  if(!active) return;
  auto pageAddress = u32(address) & ~0x1fffu;
  auto& page = pages[pageAddress];
  page.accesses++;
  store ? page.stores++ : page.loads++;
  if(result == TlbResult::CacheHit) {
    page.cacheHits++;
    tlbCacheHits++;
  } else {
    page.cacheMisses++;
    tlbCacheMisses++;
    if(result == TlbResult::Missing) {
      page.missing++;
      tlbMissing++;
    }
  }
}

auto CPU::Profiler::capturePath(const char* suffix) const -> std::filesystem::path {
  std::ostringstream name;
  name << outputPrefix.string() << '-' << std::setfill('0') << std::setw(3) << captureSequence << suffix;
  return name.str();
}

auto CPU::Profiler::csv(const std::string& value) -> std::string {
  std::string result = "\"";
  for(auto character : value) result += character == '"' ? "\"\"" : std::string(1, character);
  return result + '"';
}

auto CPU::Profiler::writeCapture() -> void {
  auto summaryPath = capturePath("-summary.csv");
  if(auto parent = summaryPath.parent_path(); !parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
  }

  u64 frameTotal = 0;
  for(auto& frame : frames) frameTotal += frame.endCycle - frame.startCycle;

  {
    std::ofstream output(summaryPath);
    output << "metric,value\n"
           << "stage," << captureStage << "\n"
           << "start_cycle," << captureStartCycle << "\n"
           << "end_cycle," << captureEndCycle << "\n"
           << "total_cycles," << captureEndCycle - captureStartCycle << "\n"
           << "frames," << frames.size() << "\n"
           << "average_frame_delta," << (frames.empty() ? 0 : frameTotal / frames.size()) << "\n"
           << "tlb_cache_hits," << tlbCacheHits << "\n"
           << "tlb_cache_misses," << tlbCacheMisses << "\n"
           << "tlb_missing," << tlbMissing << "\n";
  }

  {
    std::ofstream output(capturePath("-functions.csv"));
    output << "address,size,name,calls,self_cycles,inclusive_cycles\n";
    for(auto& function : functions) {
      if(!function.calls && !function.selfCycles) continue;
      output << "0x" << std::hex << std::setw(8) << std::setfill('0') << function.address
             << std::dec << ',' << function.size << ',' << csv(function.name) << ','
             << function.calls << ',' << function.selfCycles << ',' << function.inclusiveCycles << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-tlb.csv"));
    output << "page,accesses,loads,stores,cache_hits,cache_misses,missing\n";
    for(auto& [address, page] : pages) {
      output << "0x" << std::hex << std::setw(8) << std::setfill('0') << address << std::dec
             << ',' << page.accesses << ',' << page.loads << ',' << page.stores << ','
             << page.cacheHits << ',' << page.cacheMisses << ',' << page.missing << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-frames.csv"));
    output << "frame,start_cycle,end_cycle,delta_cycles\n";
    for(size_t index = 0; index < frames.size(); index++) {
      auto& frame = frames[index];
      output << index << ',' << frame.startCycle << ',' << frame.endCycle << ','
             << frame.endCycle - frame.startCycle << "\n";
    }
  }

  {
    std::ofstream output(capturePath("-game-frames.csv"));
    output << "frame,tick_cycles,tlb_loads,start_cycle,end_cycle\n";
    for(size_t index = 0; index < gameFrames.size(); index++) {
      auto& frame = gameFrames[index];
      // VR4300 CP0 Count (used by osGetCount) advances once per two CPU cycles.
      output << index << ',' << (frame.endCycle - frame.startCycle) / 2 << ',' << frame.tlbLoads
             << ',' << frame.startCycle << ',' << frame.endCycle << "\n";
    }
  }

  {
    std::ofstream output(capturePath(".folded"));
    for(auto& [callstack, count] : folded) output << callstack << ' ' << count << "\n";
  }

  std::fprintf(stderr, "ares N64 profiler: capture %u written:\n", captureSequence);
  std::fprintf(stderr, "  %s\n", summaryPath.string().c_str());
  std::fprintf(stderr, "  %s\n", capturePath("-functions.csv").string().c_str());
  std::fprintf(stderr, "  %s\n", capturePath("-tlb.csv").string().c_str());
  std::fprintf(stderr, "  %s\n", capturePath("-frames.csv").string().c_str());
  std::fprintf(stderr, "  %s (%zu GoldenEye replay frames)\n",
               capturePath("-game-frames.csv").string().c_str(), gameFrames.size());
  std::fprintf(stderr, "  %s\n", capturePath(".folded").string().c_str());
}
