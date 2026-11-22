#!/bin/bash
# CI TANI (yalnız ci/mtlgl-ios-ar dalı), dördüncü koşu: eski davranış (glCopyImageSubData / glClearBufferfv boş geçilir: a42424d6 öncesi)
# ile yeni davranış sırayla üçer kez, her seferinde yeniden açılmış simülatörde ve ana CI'daki gibi 3D oyun arkada açıkken; sonra 3D oyun
# olmadan iki kez ve 3D oyun kapatıldıktan sonra bir kez. Yavaş karelerde o karede kaç yeni pipeline / doku oluşturulduğu yazılır.
# Üçüncü koşuda bir grep, simülatörün içinde süren bir yardımcının açık tuttuğu boruyu 47 dakika bekledi: simülatör komutlarının çıktısı
# artık önce dosyaya yazılır.
set +e
# macOS'ta GNU timeout yok: komut kendi süreç grubunda çalışır, süre dolunca tüm grup durdurulur (GNU timeout gibi çıkış 124)
tmo() {
  perl -e '
    my $t = shift @ARGV;
    my $pid = fork();
    die "fork failed\n" unless defined $pid;
    if ($pid == 0) { setpgrp(0, 0); exec { $ARGV[0] } @ARGV; print STDERR "tmo: cannot run $ARGV[0]: $!\n"; exit 127; }
    eval { local $SIG{ALRM} = sub { die "alarm\n" }; alarm $t; waitpid($pid, 0); alarm 0; };
    if ($@ eq "alarm\n") { kill "TERM", -$pid; sleep 3; kill "KILL", -$pid; waitpid($pid, 0); exit 124; }
    exit($? & 127 ? 128 + ($? & 127) : $? >> 8);
  ' "$@"
}
tmo 5 true || { echo "::error title=tmo::the time limit helper does not work"; exit 1; }
mkdir -p diag
esc() { sed -e 's/%/%25/g' | tr '\n' ' ' | cut -c1-1500; }

basic=$(ls -d ~/.cache/cq-build/regression/Reg_ios_simulator/Builds/iOS/*.app | head -1)
app=$(ls -d ~/.cache/cq-build/regression/Reg_ios_simulator_ar/Builds/iOS/*.app | head -1)
basic_id=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$basic/Info.plist")
bid=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$app/Info.plist")
tmo 60 xcrun simctl list devices available -j > diag/devices.json 2>/dev/null
udid=$(python3 -c "import json,sys; d=json.load(open('diag/devices.json'))['devices']; p=[x for r,ds in d.items() if 'iOS' in r for x in ds if 'iPhone' in x.get('name','')]; print(p[-1]['udid'])")
echo "device $udid | basic $basic_id | ar $bid" | tee -a diag/summary.txt

fresh() {
  tmo 120 xcrun simctl shutdown all > /dev/null 2>&1
  tmo 300 xcrun simctl boot "$udid" > /dev/null 2>&1
  tmo 300 xcrun simctl bootstatus "$udid" -b > /dev/null 2>&1
}

# 3D oyun bir kez kurulur (simulator_test.py kurar ve çalıştırır)
fresh
tmo 400 python3 Tools/iOS/simulator_test.py "$basic" --screenshot diag/basic.png --wait 45 > diag/basic.out 2>&1
bline="exit $? | $(grep -m1 'Screenshot' diag/basic.out | sed 's/.*Screenshot/Screenshot/')"
echo "basic 3D game: $bline" | tee -a diag/summary.txt
echo "::notice title=basic 3D game::$(echo "$bline" | esc)"

# variant <ad> <3D oyun: 1 arkada açık, 0 yok, k açılıp kapatılır> [ortam...]
variant() {
  name=$1
  withbasic=$2
  shift 2
  t0=$(date +%s)
  fresh
  if [ "$withbasic" != 0 ]; then
    tmo 60 xcrun simctl launch "$udid" "$basic_id" > /dev/null 2>&1
    sleep 30
    if [ "$withbasic" = k ]; then
      tmo 30 xcrun simctl terminate "$udid" "$basic_id" > /dev/null 2>&1
      sleep 5
    fi
  fi
  tmo 420 env "$@" python3 Tools/iOS/simulator_test.py "$app" --screenshot "diag/$name.png" --wait 45 --expect-log "ARKit" > "diag/$name.out" 2>&1
  code=$?
  tmo 30 xcrun simctl spawn "$udid" launchctl list > "diag/$name.launchctl" 2>/dev/null
  basic_alive=$(grep -c "$basic_id" "diag/$name.launchctl")
  tmo 30 xcrun simctl terminate "$udid" "$bid" > /dev/null 2>&1
  tmo 30 xcrun simctl terminate "$udid" "$basic_id" > /dev/null 2>&1
  tmo 120 xcrun simctl spawn "$udid" log show --last 4m --style compact --predicate 'process == "conqueror"' > "diag/$name.oslog.full" 2>&1
  grep -h "MTLCommandBufferErrorDomain\|XPC service was lost\|XPC_ERROR\|SimMetalHost" "diag/$name.oslog.full" | tail -40 > "diag/$name.oslog"
  rm -f "diag/$name.oslog.full"
  glog="diag/$name.log"
  crashed=$(grep -c "crashed ==========" "$glog" 2>/dev/null)
  cbfail=$(grep -c "\[Metal diag\] command buffer failed" "$glog" 2>/dev/null)
  slow=$(grep -c "\[Metal diag\] slow" "$glog" 2>/dev/null)
  slowmax=$(grep -h "\[Metal diag\] slow" "$glog" 2>/dev/null | sed -n 's/.*GPU \([0-9.]*\) ms.*/\1/p' | sort -n | tail -1)
  oserr=$(grep -c "MTLCommandBufferErrorDomain" "diag/$name.oslog" 2>/dev/null)
  xpc=$(grep -c "XPC service was lost" "diag/$name.oslog" 2>/dev/null)
  shot=$(grep -m1 'Screenshot' "diag/$name.out" | sed 's/.*Screenshot/Screenshot/')
  dt=$(( $(date +%s) - t0 ))
  line="exit $code | 3D game $withbasic (alive at end $basic_alive) | crash $crashed | cb failed $cbfail (os $oserr) | SimMetalHost lost $xpc | slow $slow (max ${slowmax:-0} ms) | ${dt}s | $shot | $*"
  echo "$name: $line" | tee -a diag/summary.txt
  echo "::notice title=$name::$(echo "$line" | esc)"
  detail=$( (grep -h "\[Metal diag\] slow\|\[Metal diag\] command buffer failed\|crashed ====\|Signal:" "$glog" 2>/dev/null | sed 's/^\[[0-9:]*\] \[T:[0-9]*\] \[CORE\] //' | head -10; \
             grep -h "\[Metal diag\] frame [0-9]* resources" "$glog" 2>/dev/null | sed 's/^\[[0-9:]*\] \[T:[0-9]*\] \[CORE\] //' | head -12) | cut -c1-220)
  if [ -n "$detail" ]; then
    echo "$detail" | sed 's/^/    /' | tee -a diag/summary.txt
    echo "::warning title=$name detail::$(echo "$detail" | esc)"
  fi
}

# Beşinci koşu: yeni davranış ile yalnız TAA'nın önceki kare derinliği kopyası atlanmış hali sırayla dörder kez (3D oyun arkada açık,
# ana CI'daki gibi). Dördüncü koşuda yeni davranışta 6 koşunun 4'ünde zaman aşımı, 2'sinde SimMetalHost yeniden başlaması; eskide 3'te 1 zaman aşımı
NODEPTH="SIMCTL_CHILD_CQ_GLEMU_SKIP_DEPTHCOPY=1"
variant new_1 1 SIMCTL_CHILD_CQ_GLEMU_NONE=1
variant nodepth_1 1 $NODEPTH
variant new_2 1 SIMCTL_CHILD_CQ_GLEMU_NONE=1
variant nodepth_2 1 $NODEPTH
variant new_3 1 SIMCTL_CHILD_CQ_GLEMU_NONE=1
variant nodepth_3 1 $NODEPTH
variant new_4 1 SIMCTL_CHILD_CQ_GLEMU_NONE=1
variant nodepth_4 1 $NODEPTH

tmo 120 xcrun simctl shutdown all > /dev/null 2>&1
cat diag/summary.txt >> "$GITHUB_STEP_SUMMARY"
exit 0
