#!/bin/bash
# CI TANI (yalnız ci/mtlgl-ios-ar dalı): AR oyunu her seçenekle bir kez, her seferinde yeniden açılmış simülatörde (önceki seçeneğin
# SimMetalHost çökmesi sonrakini etkilemesin). Sonuç satırları ayrıca GitHub uyarı notu (annotation) olarak yazılır: ilk tanı
# koşusunda koşucu sunucuyla bağlantısını kaybetmiş, günlük ve dosyalar yüklenmemişti.
set +e
mkdir -p diag
basic=$(ls -d ~/.cache/cq-build/regression/Reg_ios_simulator/Builds/iOS/*.app | head -1)
app=$(ls -d ~/.cache/cq-build/regression/Reg_ios_simulator_ar/Builds/iOS/*.app | head -1)
basic_id=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$basic/Info.plist")
bid=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$app/Info.plist")
udid=$(xcrun simctl list devices available -j | python3 -c "import json,sys; d=json.load(sys.stdin)['devices']; p=[x for r,ds in d.items() if 'iOS' in r for x in ds if 'iPhone' in x.get('name','')]; print(p[-1]['udid'])")
echo "device $udid | basic $basic_id | ar $bid" | tee -a diag/summary.txt

esc() { sed -e 's/%/%25/g' | tr '\n' ' ' | cut -c1-1500; }

fresh() {
  timeout 120 xcrun simctl shutdown all > /dev/null 2>&1
  timeout 300 xcrun simctl boot "$udid" > /dev/null 2>&1
  timeout 300 xcrun simctl bootstatus "$udid" -b > /dev/null 2>&1
}

# 3D oyun bir kez kurulur (simulator_test.py kurar ve çalıştırır)
fresh
timeout 400 python3 Tools/iOS/simulator_test.py "$basic" --screenshot diag/basic.png --wait 45 > diag/basic.out 2>&1
echo "basic 3D game: exit $? | $(grep -m1 'Screenshot' diag/basic.out | sed 's/.*Screenshot/Screenshot/')" | tee -a diag/summary.txt

variant() {
  name=$1
  withbasic=$2
  shift 2
  t0=$(date +%s)
  fresh
  touch "diag/.t_$name"
  if [ "$withbasic" = 1 ]; then
    timeout 60 xcrun simctl launch "$udid" "$basic_id" > /dev/null 2>&1
    sleep 30
  fi
  env "$@" timeout 420 python3 Tools/iOS/simulator_test.py "$app" --screenshot "diag/$name.png" --wait 45 --expect-log "ARKit" > "diag/$name.out" 2>&1
  code=$?
  basic_alive=$(timeout 30 xcrun simctl spawn "$udid" launchctl list 2>/dev/null | grep -c "$basic_id")
  timeout 30 xcrun simctl terminate "$udid" "$bid" > /dev/null 2>&1
  timeout 30 xcrun simctl terminate "$udid" "$basic_id" > /dev/null 2>&1
  timeout 150 xcrun simctl spawn "$udid" log show --last 3m --style compact --predicate 'process == "conqueror"' 2>&1 | tail -c 3000000 > "diag/$name.oslog"
  timeout 150 log show --last 4m --style compact --predicate 'process CONTAINS "SimMetalHost" OR eventMessage CONTAINS "SimMetalHost"' 2>&1 | tail -c 2000000 > "diag/$name.hostlog"
  container=$(timeout 30 xcrun simctl get_app_container "$udid" "$bid" data 2>/dev/null)
  mkdir -p "diag/$name"
  if [ -n "$container" ] && [ -d "$container" ]; then
    find "$container" -name "*.log" -size -20M -exec cp {} "diag/$name/" \;
    find "$container" -path "*crash_dumps*" -type f -exec mv {} "diag/$name/" \;
  fi
  find ~/Library/Logs/DiagnosticReports -type f -newer "diag/.t_$name" \( -name "SimMetalHost*" -o -name "conqueror*" \) -exec cp {} "diag/$name/" \; 2>/dev/null
  glog="diag/$name.log"
  crashed=$(grep -c "crashed ==========" "$glog" 2>/dev/null)
  cbfail=$(grep -c "\[Metal diag\] command buffer failed" "$glog" 2>/dev/null)
  slow=$(grep -c "\[Metal diag\] slow" "$glog" 2>/dev/null)
  oserr=$(grep -c "MTLCommandBufferErrorDomain" "diag/$name.oslog" 2>/dev/null)
  xpc=$(grep -c "XPC service was lost" "diag/$name.oslog" 2>/dev/null)
  val=$(grep -c -i "MTLDebug\|failed assertion\|validation error" "diag/$name.oslog" "$glog" 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')
  hostlines=$(grep -c "" "diag/$name.hostlog" 2>/dev/null)
  shot=$(grep -m1 'Screenshot' "diag/$name.out" | sed 's/.*Screenshot/Screenshot/')
  dt=$(( $(date +%s) - t0 ))
  line="exit $code | 3D game alive $basic_alive | crash $crashed | cb failed $cbfail (os $oserr) | SimMetalHost lost $xpc | slow $slow | validation $val | host log $hostlines | ${dt}s | $shot | $*"
  echo "$name: $line" | tee -a diag/summary.txt
  echo "::notice title=$name::$(echo "$line" | esc)"
  detail=$( (grep -h "\[Metal diag\]\|\[MtlGL diag\]\|Unsupported depth\|crashed ====\|Signal:" "$glog" 2>/dev/null | sort | uniq -c | sort -rn | head -8; \
             grep -h -i "MTLDebug\|failed assertion\|validation error" "diag/$name.oslog" "$glog" 2>/dev/null | head -4) | cut -c1-300)
  if [ -n "$detail" ]; then
    echo "$detail" | sed 's/^/    /' | tee -a diag/summary.txt
    echo "::warning title=$name detail::$(echo "$detail" | esc)"
  fi
  grep -h "GLEmu trace\|\[Metal diag\] frame" "$glog" 2>/dev/null | sort -u | head -40 | sed 's/^/    /' >> diag/summary.txt
}

variant baseline 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1
variant nodepthcopy 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1 SIMCTL_CHILD_CQ_GLEMU_SKIP_DEPTHCOPY=1
variant depthcopy_buffer 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1 SIMCTL_CHILD_CQ_MTLGL_DEPTHCOPY=buffer
variant depthcopy_draw 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1 SIMCTL_CHILD_CQ_MTLGL_DEPTHCOPY=draw
variant noclear 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1 SIMCTL_CHILD_CQ_GLEMU_SKIP_CLEARBUFFER=1
variant isolate 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1 SIMCTL_CHILD_CQ_GLEMU_ISOLATE=1
variant validation 1 SIMCTL_CHILD_MTL_DEBUG_LAYER=1 SIMCTL_CHILD_MTL_DEBUG_LAYER_ERROR_MODE=nslog SIMCTL_CHILD_MTL_DEBUG_LAYER_WARNING_MODE=nslog
variant alone 0 SIMCTL_CHILD_CQ_GLEMU_TRACE=1
variant validation_buffer 1 SIMCTL_CHILD_CQ_MTLGL_DEPTHCOPY=buffer SIMCTL_CHILD_MTL_DEBUG_LAYER=1 SIMCTL_CHILD_MTL_DEBUG_LAYER_ERROR_MODE=nslog SIMCTL_CHILD_MTL_DEBUG_LAYER_WARNING_MODE=nslog
variant baseline_again 1 SIMCTL_CHILD_CQ_GLEMU_TRACE=1

timeout 120 xcrun simctl shutdown all > /dev/null 2>&1
cat diag/summary.txt >> "$GITHUB_STEP_SUMMARY"
exit 0
