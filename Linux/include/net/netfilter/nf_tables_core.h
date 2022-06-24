/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_NF_TABLES_CORE_H
#define _NET_NF_TABLES_CORE_H

#include <net/netfilter/nf_tables.h>
#include <linux/indirect_call_wrapper.h>

extern struct nft_expr_type nft_imm_type;
extern struct nft_expr_type nft_cmp_type;
extern struct nft_expr_type nft_counter_type;
extern struct nft_expr_type nft_lookup_type;
extern struct nft_expr_type nft_bitwise_type;
extern struct nft_expr_type nft_byteorder_type;
extern struct nft_expr_type nft_payload_type;
extern struct nft_expr_type nft_dynset_type;
extern struct nft_expr_type nft_range_type;
extern struct nft_expr_type nft_meta_type;
extern struct nft_expr_type nft_rt_type;
extern struct nft_expr_type nft_exthdr_type;
extern struct nft_expr_type nft_last_type;

#ifdef CONFIG_NETWORK_SECMARK
extern struct nft_object_type nft_secmark_obj_type;
#endif
extern struct nft_object_type nft_counter_obj_type;

int nf_tables_core_module_init(void);
void nf_tables_core_module_exit(void);

struct nft_bitwise_fast_expr {
	u32			mask;
	u32			xor;
	u8			sreg;
	u8			dreg;
};

struct nft_cmp_fast_expr {
	u32			data;
	u32			mask;
	u8			sreg;
	u8			len;
	bool			inv;
};

struct nft_cmp16_fast_expr {
	struct nft_data		data;
	struct nft_data		mask;
	u8			sreg;
	u8			len;
	bool			inv;
};

struct nft_immediate_expr {
	struct nft_data		data;
	u8			dreg;
	u8			dlen;
};

/* Calculate the mask for the nft_cmp_fast expression. On big endian the
 * mask needs to include the *upper* bytes when interpreting that data as
 * something smaller than the full u32, therefore a cpu_to_le32 is done.
 */
static inline u32 nft_cmp_fast_mask(unsigned int len)
{
	return cpu_to_le32(~0U >> (sizeof_field(struct nft_cmp_fast_expr,
						data) * BITS_PER_BYTE - len));
}

extern const struct nft_expr_ops nft_cmp_fast_ops;
extern const struct nft_expr_ops nft_cmp16_fast_ops;

struct nft_payload {
	enum nft_payload_bases	base:8;
	u8			offset;
	u8			len;
	u8			dreg;
};

struct nft_payload_set {
	enum nft_payload_bases	base:8;
	u8			offset;
	u8			len;
	u8			sreg;
	u8			csum_type;
	u8			csum_offset;
	u8			csum_flags;
};

extern const struct nft_expr_ops nft_payload_fast_ops;

extern const struct nft_expr_ops nft_bitwise_fast_ops;

extern struct static_key_false nft_counters_enabled;
extern struct static_key_false nft_trace_enabled;

extern const struct nft_set_type nft_set_rhash_type;
extern const struct nft_set_type nft_set_hash_type;
extern const struct nft_set_type nft_set_hash_fast_type;
extern const struct nft_set_type nft_set_rbtree_type;
extern const struct nft_set_type nft_set_bitmap_type;
extern const struct nft_set_type nft_set_pipapo_type;
extern const struct nft_set_type nft_set_pipapo_avx2_type;

#ifdef CONFIG_RETPOLINE
bool nft_rhash_lookup(const struct net *net, const struct nft_set *set,
		      const u32 *key, const struct nft_set_ext **ext);
bool nft_rbtree_lookup(const struct net *net, const struct nft_set *set,
		       const u32 *key, const struct nft_set_ext **ext);
bool nft_bitmap_lookup(const struct net *net, const struct nft_set *set,
		       const u32 *key, const struct nft_set_ext **ext);
bool nft_hash_lookup_fast(const struct net *net,
			  const struct nft_set *set,
			  const u32 *key, const struct nft_set_ext **ext);
bool nft_hash_lookup(const struct net *net, const struct nft_set *set,
		     const u32 *key, const struct nft_set_ext **ext);
bool nft_set_do_lookup(const struct net *net, const struct nft_set *set,
		       const u32 *key, const struct nft_set_ext **ext);
#else
static inline bool
nft_set_do_lookup(const struct net *net, const struct nft_set *set,
		  const u32 *key, const struct nft_set_ext **ext)
{
	return set->ops->lookup(net, set, key, ext);
}
#endif

/* called from nft_pipapo_avx2.c */
bool nft_pipapo_lookup(const struct net *net, const struct nft_set *set,
		       const u32 *key, const struct nft_set_ext **ext);
/* called from nft_set_pipapo.c */
bool nft_pipapo_avx2_lookup(const struct net *net, const struct nft_set *set,
			    const u32 *key, const struct nft_set_ext **ext);

void nft_counter_init_seqcount(void);

struct nft_expr;
struct nft_regs;
struct nft_pktinfo;
void nft_meta_get_eval(const struct nft_expr *expr,
		       struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_cmp_eval(const struct nft_expr *expr,
		  struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_lookup_eval(const struct nft_expr *expr,
		     struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_payload_eval(const struct nft_expr *expr,
		      struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_immediate_eval(const struct nft_expr *expr,
			struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_bitwise_eval(const struct nft_expr *expr,
		      struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_range_eval(const struct nft_expr *expr,
		    struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_byteorder_eval(const struct nft_expr *expr,
			struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_dynset_eval(const struct nft_expr *expr,
		     struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_rt_get_eval(const struct nft_expr *expr,
		     struct nft_regs *regs, const struct nft_pktinfo *pkt);
void nft_counter_eval(const struct nft_expr *expr, struct nft_regs *regs,
                      const struct nft_pktinfo *pkt);
#endif /* _NET_NF_TABLES_CORE_H */
