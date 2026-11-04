from pathlib import Path
import hashlib,json,re,subprocess,tempfile

R=Path(__file__).resolve().parent
child=(R/'../modules/drivers/soc/qcom/qti_battery_debug.c').read_text()
parent=(R/'../modules/drivers/soc/qcom/qti_pmic_glink.c').read_text()
p=parent[parent.index('struct pmic_glink_reply_fence'):parent.index('static void pmic_glink_notify_clients')]
c=child[child.index('#define MSG_OWNER_BD'):child.index('static const struct of_device_id')]
h=(R/'tests/battery_debug_harness.c').read_text()
code=h.replace('/* PARENT */',p).replace('/* CHILD */',c).replace('/* UNREGISTER */',parent[parent.index('int pmic_glink_unregister_client('):parent.index('EXPORT_SYMBOL(pmic_glink_unregister_client);')])
with tempfile.TemporaryDirectory(prefix='onyx-battery-debug-') as temp:
    d=Path(temp)
    def run(source):
        (d/'test.c').write_text(source)
        subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(d/'test.c'),'-o',str(d/'test')],check=True)
        return subprocess.run([str(d/'test')],capture_output=True,text=True,timeout=40)
    result=run(code)
    assert result.returncode==0,result.stderr
    mutations={}
    variants={
        'short-stock-aggregator-request':code.replace('u32 reserved;', ''),
        'nonzero-stock-aggregator-word':code.replace('struct votables_list_req_msg req_msg = { 0 };', 'struct votables_list_req_msg req_msg = { .reserved = 1 };'),
        'ignore-qbg-payload-extent':code.replace('response->length > len - offsetof(struct qbg_context_resp_msg, buf)','false'),
        'discard-rejected-reply-fence':code.replace('fence->received = false;', 'fence->received = true;'),
        'ignore-requested-votable-id':code.replace('vote.votable_id != bd->pending_id','false'),
        'ignore-orphan-key':code.replace('valid = key == fence->key;', 'valid = true;'),
        'route-detached-reply-to-replacement':code.replace('idr_find(&pgdev->client_idr, fence->owner) != fence->client', 'false'),
        'orphan-before-callback-drain':code.replace('synchronize_srcu(&pgdev->client_srcu);', '').replace('pmic_glink_orphan_requests(pgdev, client);\n\tmutex_unlock(&pgdev->client_lock);', 'pmic_glink_orphan_requests(pgdev, client);\n\tmutex_unlock(&pgdev->client_lock);\n\tsynchronize_srcu(&pgdev->client_srcu);'),
        'retain-failed-submit-fence':code.replace('if (send_failed)','if (send_failed && rc != -ENOTCONN)'),
        'drop-full-count-boundary':code.replace('all->num_votables > MAX_NUM_VOTABLES','all->num_votables >= MAX_NUM_VOTABLES'),
        'release-lock-before-formatting':code.replace('voter_mask = v->data.active_voter_mask;', 'mutex_unlock(&bd->lock); voter_mask = v->data.active_voter_mask;'),
        'unprotected-override-file':code.replace('debugfs_create_file("override",','debugfs_create_file_unsafe("override",'),
    }
    variants.update({
      'hide-protocol-error-as-timeout':code.replace('if (!bd->stopping && bd->pending && bd->pending_opcode == hdr.opcode)', 'if (!rc && !bd->stopping && bd->pending && bd->pending_opcode == hdr.opcode)'),
      'release-rejected-fence':code.replace('pmic_glink_request_reject(bd->client, hdr.opcode);', 'pmic_glink_request_end(bd->client, hdr.opcode);'),
      'complete-unrelated-opcode':code.replace('&& bd->pending_opcode == hdr.opcode', ''),
      'publish-invalid-vote':code.replace('if (!rc) {\n\t\t\tif (hdr.opcode', 'if (true) {\n\t\t\tif (hdr.opcode'),
      'drop-qbg-after-aggregator-failure':code.replace('if (rc == -ETIMEDOUT || rc == -EAGAIN || rc == -EPROTO || rc == -ENOTCONN)', 'if (false)'),
      'hide-aggregation-error-status':code.replace('bd->votables_error = IS_ENABLED(CONFIG_DEBUG_FS) ? rc : -EOPNOTSUPP;', 'bd->votables_error = 0;'),
      'release-guard-on-availability-fallback':code.replace('QBG interface retained\\n", rc);', 'QBG interface retained\\n", rc); pmic_glink_request_end(bd->client, BD_GET_AGGREGATOR_INFO_REQ);'),
    })
    for name,source in variants.items():
        assert source!=code,name
        try:
            r=run(source)
        except subprocess.TimeoutExpired:
            mutations[name]='REJECTED: deadlock'
            continue
        assert r.returncode,name
        mutations[name]='REJECTED'
report=dict(status='PASS',cases=int(result.stdout),child_sha256=hashlib.sha256(child.encode()).hexdigest(),parent_sha256=hashlib.sha256(parent.encode()).hexdigest(),
    harness_sha256=hashlib.sha256(h.encode()).hexdigest(),sanitizers=['ASan','UBSan'],regression_mutations=mutations,
    limits=['Complete native battery driver C and actual transport reply-fence/unregister C; debugfs, sysfs, devres, RPMSG and callback drain are mocked.',
            'Native charging override and QBG commands exercised only against host mocks; no live charging override, firmware malformed packet or SSR.'])
(R/'battery-debug-host-validation.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
