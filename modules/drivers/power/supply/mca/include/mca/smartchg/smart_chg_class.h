#ifndef _MCA_SMARTCHG_SMART_CHG_CLASS_H_
#define _MCA_SMARTCHG_SMART_CHG_CLASS_H_

#include <linux/types.h>

/* Decoded host records; never cast producer wire bytes to these structs. */
struct smart_batt_spec_curve {
	int mv;
	int ma_h;
	int ma_l;
};

struct smart_batt_jeita_term_para {
	struct {
		int idx;
		int min;
		int max;
	} t_range;
	int vterm;
	int iterm;
};

struct smart_batt_spec {
	u32 type;
	u32 ffc;
	struct {
		int idx;
		int min;
		int max;
	} t_range;
	u32 step_size;
	const struct smart_batt_spec_curve *steps;
};

/*
 * Stock BASP wire ABI: nine little-endian u32s, checksum first. JEITA
 * records are five s32s. Curve prefixes are six u32/s32s followed by
 * inline (mV, mA_h, mA_l) triples, without a serialized pointer.
 */
#define MCA_BASP_HEADER_SIZE 36
#define MCA_BASP_MAX_SIZE 4096
#define MCA_BASP_MAX_STEPS 8

struct mca_basp_update {
	const struct smart_batt_jeita_term_para *jeita;
	const struct smart_batt_spec *spec;
	u32 ffc_size;
	u32 normal_size;
};

enum mca_smartchg_if_chg_type {
	MCA_SMARTCHG_IF_CHG_TYPE_BUCK = 0,
	MCA_SMARTCHG_IF_CHG_TYPE_QC,
	MCA_SMARTCHG_IF_CHG_TYPE_JEITA,
	MCA_SMARTCHG_IF_CHG_TYPE_THERMAL,
	MCA_SMARTCHG_IF_CHG_TYPE_WL_BUCK,
	MCA_SMARTCHG_IF_CHG_TYPE_WL_QC,
	MCA_SMARTCHG_IF_CHG_TYPE_END,
};

struct mca_smartchg_if_ops {
	int type;
	void *data;
	int (*set_delta_fv)(void *data, int val);
	int (*set_delta_ichg)(void *data, int val);
	int (*set_fcc)(void *data, int val);
	int (*set_pwr_boost_sts)(void *data, int en);
	int (*set_soc_limit_sts)(void *data, int en);
	int (*set_wls_quiet_sts)(void *data, int en);
	int (*set_wls_super_sts)(void *data, int en);
	/* Prepare allocates/validates without changing live policy. Commit cannot
	 * fail after a successful prepare; abort releases prepared resources.
	 * The caller holds provider SRCU across all three phases.
	 */
	int (*prepare_baa_para)(void *data, const struct mca_basp_update *update);
	void (*commit_baa_para)(void *data);
	void (*abort_baa_para)(void *data);
};

int mca_smartchg_if_ops_register(struct mca_smartchg_if_ops *ops);
/* Unregister before freeing callback data; not callable from a callback. */
void mca_smartchg_if_ops_unregister(unsigned int type, void *data);
void mca_smartchg_set_scene(int scene);
int mca_smartchg_get_scene(void);
void mca_smartchg_set_board_temp(int board_temp);
int mca_smartchg_get_board_temp(void);
int mca_smartchg_is_extreme_cold_enabled(void);
int mca_smartchg_get_limit_soc(void);

#endif /* _MCA_SMARTCHG_SMART_CHG_CLASS_H_ */
