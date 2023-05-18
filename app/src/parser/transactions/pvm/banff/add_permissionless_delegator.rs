/*******************************************************************************
*   (c) 2023 Zondax AG
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/
use bolos::{pic_str, PIC};
use core::{mem::MaybeUninit, ptr::addr_of_mut};
use nom::bytes::complete::tag;
use nom::number::complete::be_u32;
use zemu_sys::ViewError;

use crate::{
    handlers::handle_ui_message,
    parser::{
        nano_avax_to_fp_str, Address, BaseTxFields, DisplayableItem, FromBytes, Header, ObjectList,
        OutputIdx, ParserError, PvmOutput, SECPOutputOwners, SubnetId, TransferableOutput,
        Validator, MAX_ADDRESS_ENCODED_LEN, PVM_ADD_PERMISSIONLESS_DELEGATOR,
    },
};

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
#[cfg_attr(test, derive(Debug))]
pub struct AddPermissionlessDelegatorTx<'b> {
    pub tx_header: Header<'b>,
    pub base_tx: BaseTxFields<'b, PvmOutput<'b>>,
    pub validator: Validator<'b>,
    pub subnet_id: SubnetId<'b>,

    pub stake: ObjectList<'b, TransferableOutput<'b, PvmOutput<'b>>>,
    // a bit-wise idx that tells what stake outputs could be displayed
    // in the ui stage.
    // this is set during the parsing stage
    renderable_out: OutputIdx,
    pub rewards_owner: SECPOutputOwners<'b>,
}

impl<'b> FromBytes<'b> for AddPermissionlessDelegatorTx<'b> {
    #[inline(never)]
    fn from_bytes_into(
        input: &'b [u8],
        out: &mut MaybeUninit<Self>,
    ) -> Result<&'b [u8], nom::Err<ParserError>> {
        crate::sys::zemu_log_stack("AddPermissionlessDelegatorTx::from_bytes_into\x00");

        let (rem, _) = tag(PVM_ADD_PERMISSIONLESS_DELEGATOR.to_be_bytes())(input)?;

        let out = out.as_mut_ptr();

        // tx header
        let tx_header = unsafe { &mut *addr_of_mut!((*out).tx_header).cast() };
        let rem = Header::from_bytes_into(rem, tx_header)?;

        // base_tx
        let base_tx = unsafe { &mut *addr_of_mut!((*out).base_tx).cast() };
        let rem = BaseTxFields::<PvmOutput>::from_bytes_into(rem, base_tx)?;

        // validator
        let validator = unsafe { &mut *addr_of_mut!((*out).validator).cast() };
        let rem = Validator::from_bytes_into(rem, validator)?;

        // SubnetId
        let subnet_id = unsafe { &mut *addr_of_mut!((*out).subnet_id).cast() };
        let rem = SubnetId::from_bytes_into(rem, subnet_id)?;

        // stake
        // check for the number of stake-outputs before parsing then as now
        // it has to be checked for the outputIdx capacity which is used
        // to tell if an output should be rendered or not.
        let (_, num_outputs) = be_u32(rem)?;
        if num_outputs > OutputIdx::BITS {
            return Err(ParserError::TooManyOutputs.into());
        }
        let stake = unsafe { &mut *addr_of_mut!((*out).stake).cast() };
        let rem = ObjectList::<TransferableOutput<PvmOutput>>::new_into(rem, stake)?;

        // valid pointers read as memory was initialized
        let staked_list = unsafe { &*stake.as_ptr() };

        let validator_stake = unsafe { (*validator.as_ptr()).weight };

        // get locked outputs amount to check for invariant
        let stake = Self::sum_stake_outputs_amount(staked_list)?;

        // Check for invariant, the locked utxos must be equals to validators' stake
        if validator_stake != stake {
            return Err(ParserError::InvalidStakingAmount.into());
        }

        // rewards_owner
        let rewards_owner = unsafe { &mut *addr_of_mut!((*out).rewards_owner).cast() };
        let rem = SECPOutputOwners::from_bytes_into(rem, rewards_owner)?;
        unsafe {
            // by default all outputs are renderable
            addr_of_mut!((*out).renderable_out).write(OutputIdx::MAX);
        }

        Ok(rem)
    }
}

impl<'b> DisplayableItem for AddPermissionlessDelegatorTx<'b> {
    fn num_items(&self) -> usize {
        // tx_info, base_tx items, validator_items(4),
        // subnet id, rewards_to,
        // stake items and fee
        1 + self.base_tx.base_outputs_num_items()
            + self.validator.num_items()
            + 1
            + self.rewards_owner.addresses.len()
            + self.num_stake_items()
            + 1
    }

    fn render_item(
        &self,
        item_n: u8,
        title: &mut [u8],
        message: &mut [u8],
        page: u8,
    ) -> Result<u8, zemu_sys::ViewError> {
        let validator_items = self.validator.num_items() as u8;
        let base_outputs_items = self.base_tx.base_outputs_num_items() as u8;
        let stake_outputs_items = self.num_stake_items() as u8;

        if item_n == 0 {
            // FIXME: truncated due to NanoS 17 character limit
            let label = pic_str!(b"AddPermlessDelega");
            title[..label.len()].copy_from_slice(label);
            let content = pic_str!(b"Transaction");
            return handle_ui_message(content, message, page);
        }

        let item_n = item_n - 1;

        // when to start rendering staked outputs
        let render_stake_outputs_at = validator_items + base_outputs_items + 1;
        let render_last_items_at = base_outputs_items + validator_items + stake_outputs_items;
        let total_items = self.num_items() as u8;

        match item_n {
            // render base_outputs
            x @ 0.. if x < base_outputs_items => self.render_base_outputs(x, title, message, page),

            // render validator items
            x if x >= base_outputs_items && x < render_stake_outputs_at - 1 => {
                let new_idx = x - base_outputs_items;
                self.validator.render_item(new_idx, title, message, page)
            }

            // render subnet id
            x if x >= base_outputs_items && x < render_stake_outputs_at => {
                self.subnet_id.render_item(0, title, message, page)
            }

            // render stake items
            x if x >= render_stake_outputs_at
                && x < (render_stake_outputs_at + stake_outputs_items) =>
            {
                let new_idx = x - render_stake_outputs_at;
                self.render_stake_outputs(new_idx, title, message, page)
            }

            // render rewards to, delegate fee and fee
            x if x >= render_last_items_at && x < total_items - 1 => {
                // normalize index to zero
                let new_idx = x - (base_outputs_items + stake_outputs_items + validator_items + 1);
                self.render_last_items(new_idx, title, message, page)
            }
            _ => Err(ViewError::NoData),
        }
    }
}

impl<'b> AddPermissionlessDelegatorTx<'b> {
    pub fn disable_output_if(&mut self, address: &[u8]) {
        // for this stake transaction, transfer information
        // is not important so even if there is only one
        // output, just hide it from the UI as long as
        // the change address match
        self.base_tx.force_disable_output(address);

        let mut idx = 0;
        let mut render = self.renderable_out;
        self.stake.iterate_with(|o| {
            // The 99.99% of the outputs contain only one address(best case),
            // In the worse case we just show every output.
            if o.num_addresses() == 1 && o.contain_address(address) {
                render ^= 1 << idx;
            }
            idx += 1;
        });
        self.renderable_out = render;
    }

    fn fee(&'b self) -> Result<u64, ParserError> {
        let sum_inputs = self.base_tx.sum_inputs_amount()?;

        let base_outputs = self.base_tx.sum_outputs_amount()?;
        let stake_outputs = Self::sum_stake_outputs_amount(&self.stake)?;

        let total_outputs = base_outputs
            .checked_add(stake_outputs)
            .ok_or(ParserError::OperationOverflows)?;

        let fee = sum_inputs
            .checked_sub(total_outputs)
            .ok_or(ParserError::OperationOverflows)?;
        Ok(fee)
    }

    fn num_stake_items(&self) -> usize {
        let mut items = 0;
        let mut idx = 0;
        self.stake.iterate_with(|o| {
            let render = self.renderable_out & (1 << idx);
            if render > 0 {
                items += o.num_items();
            }
            idx += 1;
        });
        items
    }

    fn render_base_outputs(
        &self,
        item_n: u8,
        title: &mut [u8],
        message: &mut [u8],
        page: u8,
    ) -> Result<u8, ViewError> {
        let (obj, item_idx) = self
            .base_tx
            .base_output_with_item(item_n)
            .map_err(|_| ViewError::NoData)?;

        // for base_outputs the header is Transfer
        let header = pic_str!(b"Transfer");

        self.render_output_with_header(&obj, item_idx, title, message, page, header)
    }

    fn render_stake_outputs(
        &self,
        item_n: u8,
        title: &mut [u8],
        message: &mut [u8],
        page: u8,
    ) -> Result<u8, ViewError> {
        let (obj, item_idx) = self
            .stake_output_with_item(item_n)
            .map_err(|_| ViewError::NoData)?;

        // for staking the header is Stake
        let header = pic_str!(b"Stake");

        self.render_output_with_header(&obj, item_idx, title, message, page, header)
    }

    // helper function to render any TransferableOutput<PvmOutput>,
    // either locked or normal(comming as part of base_tx_fields)
    // the rendering is the same, the only difference is that
    // locked outputs are labeled with Stake
    fn render_output_with_header(
        &'b self,
        &obj: &TransferableOutput<'b, PvmOutput<'b>>,
        item_n: u8,
        title: &mut [u8],
        message: &mut [u8],
        page: u8,
        header: &[u8],
    ) -> Result<u8, ViewError> {
        //  'Transfer' or 'Stake':
        //      '0.5 AVAX to
        //  Address:
        //      hrp + 1asxdpfsmah8wqr6m8ymfwse5e4pa9fwnvudmpn
        //  Funds locked:
        //      0.5 AVAX until 2021-05-31 21:28:00 UTC

        // get the number of items for the obj wrapped up by PvmOutput
        let num_inner_items = obj.output.num_inner_items() as _;

        // do a custom rendering of the first base_output_items
        match item_n {
            0 => {
                title[..header.len()].copy_from_slice(header);

                // render using default obj impl
                let res = obj.render_item(0, title, message, page);

                // customize the label
                title.iter_mut().for_each(|v| *v = 0);
                title[..header.len()].copy_from_slice(header);

                res
            }
            // address rendering, according to avax team 99.99% of transactions only comes with one
            // address, but we support rendering any
            x @ 1.. if x < num_inner_items => {
                // get the address index
                let address_idx = x - 1;
                let address = obj
                    .output
                    .get_address_at(address_idx as usize)
                    .ok_or(ViewError::NoData)?;
                // render encoded address with proper hrp,
                let t = pic_str!(b"Address");
                title[..t.len()].copy_from_slice(t);

                let hrp = self.tx_header.hrp().map_err(|_| ViewError::Unknown)?;
                let mut encoded = [0; MAX_ADDRESS_ENCODED_LEN];

                let addr_len = address
                    .encode_into(hrp, &mut encoded[..])
                    .map_err(|_| ViewError::Unknown)?;

                handle_ui_message(&encoded[..addr_len], message, page)
            }
            // by default we call the objects impl here,
            // if it is a locked output, that info will be shown otherwise,
            // this returns an error
            _ => obj.render_item(item_n, title, message, page),
        }
    }

    fn sum_stake_outputs_amount(
        stake: &'b ObjectList<'b, TransferableOutput<PvmOutput<'b>>>,
    ) -> Result<u64, ParserError> {
        stake
            .iter()
            .filter_map(|output| output.amount())
            .try_fold(0u64, |acc, x| acc.checked_add(x))
            .ok_or(ParserError::OperationOverflows)
    }

    fn render_rewards_to(
        &self,
        addr_idx: usize,
        title: &mut [u8],
        message: &mut [u8],
        page: u8,
    ) -> Result<u8, zemu_sys::ViewError> {
        let label = pic_str!(b"Rewards to");
        title[..label.len()].copy_from_slice(label);

        // render owner addresses
        if let Some(addr) = self.rewards_owner.addresses.get(addr_idx) {
            let hrp = self.tx_header.hrp().map_err(|_| ViewError::Unknown)?;

            let mut address = MaybeUninit::uninit();
            Address::from_bytes_into(addr, &mut address).map_err(|_| ViewError::Unknown)?;

            let mut encoded = [0; MAX_ADDRESS_ENCODED_LEN];
            // valid read as memory was initialized
            let address = unsafe { address.assume_init() };

            let len = address
                .encode_into(hrp, &mut encoded[..])
                .map_err(|_| ViewError::Unknown)?;

            return handle_ui_message(&encoded[..len], message, page);
        }

        Err(ViewError::NoData)
    }

    fn render_last_items(
        &self,
        item_n: u8,
        title: &mut [u8],
        message: &mut [u8],
        page: u8,
    ) -> Result<u8, zemu_sys::ViewError> {
        use lexical_core::Number;

        let mut buffer = [0; u64::FORMATTED_SIZE_DECIMAL + 2];
        let num_addresses = self.rewards_owner.addresses.len() as u8;

        match item_n {
            // render rewards
            x @ 0.. if x < num_addresses => {
                self.render_rewards_to(x as usize, title, message, page)
            }
            x if x == num_addresses => {
                let label = pic_str!(b"Fee(AVAX)");
                title[..label.len()].copy_from_slice(label);

                let fee = self.fee().map_err(|_| ViewError::Unknown)?;
                let fee_buff =
                    nano_avax_to_fp_str(fee, &mut buffer[..]).map_err(|_| ViewError::Unknown)?;
                handle_ui_message(fee_buff, message, page)
            }
            _ => Err(ViewError::NoData),
        }
    }

    // Gets the obj that contain the item_n, along with the index
    // of the item. Returns an error otherwise
    pub fn stake_output_with_item(
        &'b self,
        item_n: u8,
    ) -> Result<(TransferableOutput<PvmOutput>, u8), ParserError> {
        let mut count = 0usize;
        let mut obj_item_n = 0;
        // index to check for renderable outputs.
        // we can omit this and be "fancy" with iterators but
        // they consume a lot of stack.
        // causing stack overflows in nanos
        let mut idx = 0;
        // gets the output that contains item_n
        // and its corresponding index
        let filter = |o: &TransferableOutput<'b, PvmOutput>| -> bool {
            let render = self.renderable_out & (1 << idx) > 0;
            idx += 1;
            if !render {
                return false;
            }

            let n = o.num_items();
            for index in 0..n {
                count += 1;
                obj_item_n = index;
                if count == item_n as usize + 1 {
                    return true;
                }
            }
            false
        };

        let obj = self
            .stake
            .get_obj_if(filter)
            .ok_or(ParserError::DisplayIdxOutOfRange)?;
        Ok((obj, obj_item_n as u8))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    include!("testvectors/add_permissionless_delegator.rs");

    #[test]
    fn ui_permissionless_delegator() {
        for (i, data) in [
            SAMPLE,
            SIMPLE_ADD_PERMISSIONLESS_DELEGATOR,
            // COMPLEX_ADD_PERMISSIONLESS_DELEGATOR, // sum of inputs overflows u64
            SIMPLE_ADD_SUBNET_PERMISSIONLESS_DELEGATOR,
            // COMPLEX_ADD_SUBNET_PERMISSIONLESS_DELEGATOR, // sum of inputs overflows u64
        ]
        .iter()
        .enumerate()
        {
            std::println!(
            "-------------------- Add Permissionless Delegator TX #{i} ------------------------"
        );
            let (_, tx) = AddPermissionlessDelegatorTx::from_bytes(data).unwrap();

            for i in 0..tx.num_items() {
                let mut title = [0; 100];
                let mut value = [0; 100];

                tx.render_item(i as _, title.as_mut(), value.as_mut(), 0)
                    .unwrap();
                let t = std::string::String::from_utf8_lossy(&title).into_owned();
                let t = t.trim_end_matches(|c| (c as u8) == 0);
                let v = std::string::String::from_utf8_lossy(&value).into_owned();
                let v = v.trim_end_matches(|c| (c as u8) == 0);

                std::println!("{}:", t);
                std::println!("     {}", v);
            }
        }
    }

    #[test]
    fn parse_add_permissionless_delegator() {
        let (_, tx) = AddPermissionlessDelegatorTx::from_bytes(SAMPLE).unwrap();
        assert_eq!(tx.validator.weight, 2000000000000);
        assert_eq!(
            tx.subnet_id,
            SubnetId::new(&[
                243, 8, 109, 123, 252, 53, 190, 28, 104, 219, 102, 75, 169, 206, 97, 162, 6, 1, 38,
                176, 214, 180, 191, 176, 159, 215, 165, 251, 118, 120, 202, 218
            ])
        );

        let (_, tx) =
            AddPermissionlessDelegatorTx::from_bytes(SIMPLE_ADD_PERMISSIONLESS_DELEGATOR).unwrap();
        assert_eq!(tx.validator.weight, 2000000000000);
        assert_eq!(tx.subnet_id, SubnetId::PRIMARY_NETWORK);
        assert_eq!(tx.rewards_owner.locktime, 0);

        let (_, tx) =
            AddPermissionlessDelegatorTx::from_bytes(COMPLEX_ADD_PERMISSIONLESS_DELEGATOR).unwrap();
        assert_eq!(tx.validator.weight, 5000000000000);
        assert_eq!(tx.subnet_id, SubnetId::PRIMARY_NETWORK);
        assert_eq!(
            tx.stake
                .iter()
                .nth(1)
                .expect("2 stake outs")
                .output
                .output
                .secp_transfer()
                .expect("SECP transfer")
                .locktime,
            87654321
        );

        let subnet_id = SubnetId::new(&[
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
            0x17, 0x18, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38,
        ]);

        let asset_id = &[
            0x99, 0x77, 0x55, 0x77, 0x11, 0x33, 0x55, 0x31, 0x99, 0x77, 0x55, 0x77, 0x11, 0x33,
            0x55, 0x31, 0x99, 0x77, 0x55, 0x77, 0x11, 0x33, 0x55, 0x31, 0x99, 0x77, 0x55, 0x77,
            0x11, 0x33, 0x55, 0x31,
        ];

        let (_, tx) =
            AddPermissionlessDelegatorTx::from_bytes(SIMPLE_ADD_SUBNET_PERMISSIONLESS_DELEGATOR)
                .unwrap();
        assert_eq!(tx.validator.weight, 1);
        assert_eq!(tx.subnet_id, subnet_id);
        assert_eq!(
            tx.stake.iter().next().expect("1 stake out").asset_id().id(),
            asset_id
        );
        assert_eq!(tx.rewards_owner.locktime, 0);

        let (_, tx) =
            AddPermissionlessDelegatorTx::from_bytes(COMPLEX_ADD_SUBNET_PERMISSIONLESS_DELEGATOR)
                .unwrap();
        assert_eq!(tx.validator.weight, 9);
        assert_eq!(tx.subnet_id, subnet_id);
        assert_eq!(
            tx.base_tx
                .outputs()
                .iter()
                .nth(2)
                .expect("3 outputs")
                .output
                .output
                .secp_transfer()
                .expect("secp transfer")
                .amount,
            0xfffffffffffffff0
        );
        assert_eq!(
            tx.stake
                .iter()
                .nth(1)
                .expect("2 stake outs")
                .asset_id()
                .id(),
            asset_id
        );
        assert_eq!(tx.rewards_owner.locktime, 0);
    }
}
